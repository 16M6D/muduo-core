#pragma once

#include "noncopyable.h"
#include "Thread.h"

#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>

class EventLoop;

class EventLoopThread : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
        const std::string &name = std::string());
    ~EventLoopThread();
    EventLoop* startLoop();
    void setCpuAffinity(int cpu_id);
private:
    void threadFunc();

    EventLoop* loop_;
    bool exiting_;
    Thread thread_;
    ThreadInitCallback callback_;
    std::mutex mutex_;
    std::condition_variable cond_;
};