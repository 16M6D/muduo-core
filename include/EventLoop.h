#pragma once

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <memory>

class Channel;
class Poller;

class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop(); // 开启事件循环
    void quit(); // 退出事件循环

    Timestamp pollReturnTime() const { return pollReturnTime_; }
    
    // 在当前loop中执行cb
    void runInLoop(Functor cb);
    // 把cb放入队列, 唤醒loop所在的线程, 执行cb
    void queueInLoop(Functor cb);

    // 唤醒loop所在线程
    void wakeup();

    // EventLoop -> Poller
    void updateChannel(Channel* channel); 
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    // 判断当前线程与EventLoop能不能对应
    bool isInLoopThread() const { return threadID_ == CurrentThread::tid(); } 
private:
    void handleRead(); // wake up
    void doPendingFunctors(); // 执行回调

    using ChannelList = std::vector<Channel*>;
    
    std::atomic_bool looping_;
    std::atomic_bool quit_;

    const pid_t threadID_;

    Timestamp pollReturnTime_;
    std::unique_ptr<Poller> poller_;

    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    
    ChannelList activeChannels_;

    std::atomic_bool callingPendingFunctors_;
    std::vector<Functor> pendingFunctors_; // 存储loop需要执行的所有回调操作
    std::mutex mutex_;
    
};
