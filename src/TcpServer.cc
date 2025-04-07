#include "TcpServer.h"
#include "Logger.h"
#include "Callbacks.h"
#include "TcpConnection.h"

#include <strings.h>

static EventLoop* CheckNotNull(EventLoop* loop) 
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpServer::TcpServer(EventLoop* loop
    , const InetAddress &listenAddr
    , const std::string &nameArg
    ,Option option)
    : loop_(CheckNotNull(loop))
    , ipPort_(listenAddr.toIpPort())
    , name_(nameArg)
    , acceptor_(new Acceptor(loop, listenAddr, option == kNoReusePort))
    , threadPool_(new EventLoopThreadPool(loop, name_))
    , connectionCallback_()
    , messageCallback_()
    , nextConnId_(1)
    , started_(0)
    {
        // 当有新用户连接时, 会执行newConnectionCallback
        acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
            std::placeholders::_1, std::placeholders::_2));
    }

TcpServer::~TcpServer()
{
    for (auto& item : connections_)
    {
        TcpConnectionPtr conn(item.second); // 局部的shared_ptr会自动释放资源
        item.second.reset();
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn)
        );
    }
}

int TcpServer::getCPUcores() 
{
    unsigned int cores = std::thread::hardware_concurrency();
    return cores; // 至少CPU核数设置为 1
}

// 设置subloop数
void TcpServer::setThreadNum(int numThreads)
{
    if (numThreads > 0) 
    {
        threadPool_->setThreadNum(numThreads);
    }
    else 
    {
        threadPool_->setThreadNum(getCPUcores());
    }
}

// 开启服务器监听 => 即将开始loop.loop()监听事件
void TcpServer::start()
{
    if(started_++ == 0) // 防止一个TcpServer对象被启动多次
    {
        threadPool_->start(threadInitCallback_);    // 启动所有线程
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get())); 
    }
}

// 新的客户端连接,由Acceptor执行该回调
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
    EventLoop* ioLoop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "- %s # %d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;
    LOG_INFO("TcpServer::newConnection[%s] - newConnection[%s] from %s\n",
        name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());
        
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrlen = sizeof local;
    if(::getsockname(sockfd, (sockaddr*)&local, &addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr\n");
    }
    InetAddress localAddr(local);

    // 根据连接成功的sockfd, 创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(
                                ioLoop
                                , connName
                                , sockfd
                                , localAddr
                                , peerAddr));
    connections_[connName] = conn;
    // 用户设置的回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writecompleteCallback_);
    
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1)
    );
    // 设置回调后直接调用connectEstablisher
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop[%s] - connection %s\n", 
        name_.c_str(), conn->name().c_str());
    connections_.erase(conn->name());
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn)
    );
}