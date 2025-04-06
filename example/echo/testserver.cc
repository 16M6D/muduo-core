#include <mymuduo/TcpServer.h>
#include <mymuduo/Logger.h>
#include <string>
#include <functional>

class EcoServer
{
public:
    EcoServer(EventLoop* loop, const InetAddress &addr, const std::string &nameArg) 
        :loop_(loop)
        , server_(loop, addr, nameArg)
    {
        // 注册回调函数
        server_.setConnetionCallback(std::bind(&EcoServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(std::bind(&EcoServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        
        // 设置合适的loop线程数量 one loop per thread. 1 mainloop + 3 subloop
        server_.setThreadNum(0);
    }

    void start()
    {
        server_.start();
    }
private:
    // 连接建立或者断开回调
    void onConnection(const TcpConnectionPtr &conn) 
    {
        if (conn->connected())
        {
            LOG_INFO("conn UP : %s", conn->peerAddr().toIpPort().c_str());
        }
        else
        {
            LOG_INFO("conn DOWN : %s", conn->peerAddr().toIpPort().c_str());
        }
    }
    // 可读写事件回调
    void onMessage(const TcpConnectionPtr &conn
                    , Buffer* buf
                    , Timestamp time) 
    {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg); // 发送
        conn->shutDown(); // 关闭写端, 响应EPOLLHUP, 执行closeCallback_;
    }

    EventLoop* loop_;
    TcpServer server_; // 没有默认构造
};

int main()
{
    EventLoop loop;
    InetAddress addr(8000);
    EcoServer server(&loop, addr, "EcoServer-01");
    server.start(); // 
    loop.loop(); // 启动mainLoop底层的EPollPoller, 开始监听事件
}