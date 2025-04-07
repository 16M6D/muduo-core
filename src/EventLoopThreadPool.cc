#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "TcpServer.h"
#include <memory>

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop)
    , name_(nameArg)
    , started_(false)
    , numThreads_(0)
    , next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool() {} // 什么都不用做, 因为ELT是在线程栈上运行的

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        EventLoopThread* t = new EventLoopThread(cb, buf);
        t->setCpuAffinity(i % TcpServer::getCPUcores());
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
        loops_.push_back(t->startLoop()); // 底层创建线程,绑定一个新的EventLoop,并返回Loop的地址
    }
    // 说明只有一个线程baseloop, 执行cb
    if (numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

// 多线程中baseloop以轮询方式给subloop提供channel
EventLoop* EventLoopThreadPool::getNextLoop()
{
    EventLoop* loop = baseLoop_;
    if(!loops_.empty())
    {
        loop = loops_[next_];
        ++next_;
        if (next_ >= loops_.size())
        {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return {baseLoop_};
    }
    else
    {
        return loops_;
    }
}