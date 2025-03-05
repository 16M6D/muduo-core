#pragma once

#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop; // 减少头文件的暴露

// Demultiplex == EventLoop == poller + n * channel
// Channel理解为通道， 封装了sockfd和其感兴趣的事件， 绑定了poller返回的具体事件
class Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(Timestamp)>;
    Channel(EventLoop *loop, int fd);
    ~Channel();

    // fd得到poller通知后处理事件
    void handleEvent(Timestamp receiveTime);

    // 设置回调函数
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); } // 使用移动语义减少内存拷贝开销
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }
    
    // 防止channel被remove掉后，还在执行回调
    void tie(const std::shared_ptr<void>&);

    int fd() const { return fd_; }
    int events() const { return events_; }
    int set_revents(int revt) { revents_ = revt; }

    // 让fd注册事件
    // update()就是一个epoll ctl，通知poller当前fd感兴趣的事件
    void enableReading() { events_ |= kReadEvent; update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); } // read位为0， 其他位是1， 不影响其他事件
    void enableWriting() { events_ |= kWriteEvent; update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }
    
    // 返回fd当前的事件状态
    bool isNoneEvent() const { return events_ == kNoneEvent; } // 当前channel是否注册感兴趣的事件
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; } 

    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }

    // 返回当前Channel属于的EventLoop
    EventLoop* ownerLoop() { return loop_; }
    void remove();

private:

    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_; // 事件循环
    const int fd_;    // fd, poller监听的对象
    int events_;      // 注册的fd感兴趣的事件
    int revents_;     // Channel接收到的poller返回的事件
    int index_;

    std::weak_ptr<void> tie_; // Channel被手动remove以后， 跨线程对生存状态进行监听
    bool tied_;

    // 通道内获知了fd最终发生的具体事件revents, 所以由它来负责调用具体事件的回调操作
    // 四个函数对象， 用来绑定外部传入的相关操作， 用户指定做什么事
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;

};