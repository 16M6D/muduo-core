#include <mymuduo/TcpServer.h>
#include <mymuduo/Logger.h>
#include <string>
#include <functional>
#include <cstdlib>

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
        server_.setThreadNum(3);
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
    // 可读写事件回调 — 纯 echo，不关闭连接
    void onMessage(const TcpConnectionPtr &conn
                    , Buffer* buf
                    , Timestamp time)
    {
        (void)time;
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
        // NOTE: 不再调用 shutDown(), 连接保持活跃
        // 这是 echo server 的规范行为
    }

    EventLoop* loop_;
    TcpServer server_; // 没有默认构造
};

int main(int argc, char* argv[])
{
    uint16_t port = 8000;
    if (argc > 1)
        port = static_cast<uint16_t>(atoi(argv[1]));

    EventLoop loop;
    InetAddress addr(port);
    EcoServer server(&loop, addr, "EcoServer-01");
    server.start();
    LOG_INFO("EchoServer listening on port %d", port);
    loop.loop(); // 启动mainLoop底层的EPollPoller, 开始监听事件
}