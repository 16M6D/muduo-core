#pragma once

/* 
    使用muduo库编写服务器程序
*/
#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "Callbacks.h"
#include "EventLoopThreadPool.h"
#include "TcpConnection.h"
#include "Buffer.h"
#include "Thread.h"

#include <functional>
#include <atomic>
#include <unordered_map>

class TcpServer : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    
    enum Option
    {
        kNoReusePort,
        kReusePort,
    };
    
    TcpServer(EventLoop* loop
                , const InetAddress &listenAddr
                , const std::string &nameArg
                ,Option option = kNoReusePort);
    ~TcpServer();

    const std::string& ipPort() const { return ipPort_; }
    const std::string& name() const { return name_; }
    EventLoop* getLoop() const { return loop_; } 

    // 设置各种回调函数
    void setThreadInitCallback(const ThreadInitCallback& cb) { threadInitCallback_ = cb; }
    void setConnetionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setWriteCompleteCallback(const WriteComplete& cb) { writecompleteCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }

    // 设置subloop的个数,应该与CPU核数匹配
    static int getCPUcores();
    
    void setThreadNum(int numThreads);

    // 开启服务器监听
    void start();

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn); 

    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    EventLoop* loop_; // baseloop 由用户定义
    const std::string ipPort_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_; // 运行在mainloop, 监听新连接事件
    std::shared_ptr<EventLoopThreadPool> threadPool_;

    ConnectionCallback connectionCallback_; // 有新连接时回调
    WriteComplete writecompleteCallback_; // 消息发送完成以后的回调
    MessageCallback messageCallback_; // 有读写消息时的回调
    ThreadInitCallback threadInitCallback_; // loop线程初始化时的回调
    std::atomic_int started_;
    
    int nextConnId_;
    ConnectionMap connections_; // 保存所有连接
};