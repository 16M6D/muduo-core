#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "Buffer.h"
#include "EventLoop.h"

#include <functional>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
#include <netinet/tcp.h>
#include <string>

static EventLoop* CheckNotNull(EventLoop* loop) 
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d TcpConnection loop is null!", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}


TcpConnection::TcpConnection(EventLoop* loop
                , const std::string &nameArg
                , int sockfd
                , const InetAddress& localAddr
                , const InetAddress& peerAddr)
    : loop_(CheckNotNull(loop))
    , name_(nameArg)
    , state_(kConnecting)
    , reading_(true)
    , socket_(new Socket(sockfd))
    , channel_(new Channel(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024)
{
    // 给channel设置相应的回调函数,由poller通知channel感兴趣的事件发生了,channel执行相应回调
    channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true); // TcpSocket保活机制
}



TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

// 发送数据
void TcpConnection::send(const std::string& buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));
        }
    }
}

/**
 * 执行发送数据
 * 应用写得快,而内核发送慢,需要写入缓冲区并设置水位回调
 */
void TcpConnection::sendInLoop(const void* data, size_t len)
{
    ssize_t nwrote = 0;
    ssize_t remaining = len;
    bool faultError = false;
    if (state_ == kDisconnected)
    {
        LOG_ERROR("disconnected, give up writing.\n");
        return;
    }
    // channel_第一次开始写, 且缓冲区没有待发送数据
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {   
            // 既然数据全部发送完成, 就不再给channel设置epollout事件
            remaining = len - nwrote;
            if (remaining == 0 && writeComplete_) 
            {
                // 安全地创建一个指向自己的shared_ptr,防止悬空指针造成UB
                loop_->queueInLoop(std::bind(writeComplete_, shared_from_this())); 
            }
        }
        else  // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop error.\n");
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
    // 说明当前这次write没有发送全部数据
    // 剩余数据需要保存到缓冲区中, 给channel注册epollout事件
    // poller发现tcp缓冲区有空间,会通知相应的sock-channel, 调用writeCallback_回调方法
    // 调用handleWrite方法把发送缓冲区的数据全部发送完成    
    if (!faultError && remaining > 0)  
    {
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append((char*)data + nwrote, remaining);
        if(!channel_->isWriting())
        {
            channel_->enableWriting(); // 注册channel的写事件,否则poller不会给channel调用epollout
        }
    }
}

// 关闭连接
void TcpConnection::shutDown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明发送缓冲区的数据全部发送完成
    {
        socket_->shutdownWrite(); // 关闭写端
    }
}

// 连接建立
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); // 向poller注册channel的epollin事件
    // 连接建立, 执行回调
    connectionCallback_(shared_from_this());
}
// 连接销毁
void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll();
        connectionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中移除 channel->loop->poller: channels.erase()
}

void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0)
    {
        // 已经建立连接的用户有可读事件发生,调用用户传入的回调操作
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0)
    {
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead error.\n");
        handleError();
    }
}

void TcpConnection::handleClose()
{
    LOG_INFO("fd = %d state = %d\n", channel_->fd(), (int)state_);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    connectionCallback_(guardThis); // 执行连接关闭回调
    closeCallback_(guardThis); // 关闭连接的回调
}


void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err  = errno;
    }
    else 
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d. \n", name_.c_str(), err);
}

void TcpConnection::handleWrite()
{
    if (channel_->isWriting())
    {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0) // 发送已完成
            {
                channel_->disableWriting();
                if (writeComplete_)
                {
                    // 唤醒loop_对应的thread线程, 执行回调
                    loop_->queueInLoop(
                        std::bind(writeComplete_, shared_from_this())
                    );
                }
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop(); // 删除当前connection
                }
            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
        LOG_ERROR("TcpConnection fd=%d is shutdown, no more writing.\n", channel_->fd());
    }
}
