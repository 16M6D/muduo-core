#pragma once

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"

#include <memory>
#include <string>
#include <atomic>

class Channel;
class Socket;
class EventLoop;

/**
 * TcpServer -> Acceptor: 通过accept()拿到新用户连接的fd
 * -> TcpConnection : 封装fd/设置回调函数 -> Channel -> Poller --监听到事件发生--> Channel调用回调操作
 * 
 * 对成功连接的客户端的数据链路进行封装
 *  */ 

class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(EventLoop* loop
            , const std::string &name
            , int sockfd
            , const InetAddress& localAddr
            , const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddr() const { return localAddr_; }
    const InetAddress& peerAddr() const { return peerAddr_; }

    bool connected() const { return state_ == kConnected; }
    
    // 发送数据
    void send(const std::string &buf);
    // 关闭连接
    void shutDown();
    
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setWriteCompleteCallback(const WriteComplete& cb) { writeComplete_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb, size_t highWaterMark) 
    { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

private:
    enum StateE {kDisconnected, kConnecting, kConnected, kDisconnecting};
    void setState(StateE state) { state_ = state; }
    void handleRead(Timestamp receiveTime);
    void handleClose();
    void handleError();
    void handleWrite();

    void sendInLoop(const void* message, size_t len);
    void shutdownInLoop();

    EventLoop* loop_;  // TcpConnection给一个subloop管理
    const std::string name_; // 必须进行名字的初始化
    std::atomic_int state_;
    bool reading_;
    std::unique_ptr<Socket>socket_;
    std::unique_ptr<Channel>channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_; 
    CloseCallback closeCallback_;
    WriteComplete writeComplete_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_; 
    size_t highWaterMark_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
};