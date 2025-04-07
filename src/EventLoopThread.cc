#include "EventLoopThread.h"
#include "EventLoop.h"
#include <sched.h>

EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
    const std::string &name)
    : loop_(nullptr)
    , exiting_(false)
    , thread_(std::bind(&EventLoopThread::threadFunc, this), name)
    , mutex_()
    , cond_()
    , callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit();
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop()
{
    thread_.start(); // 启动底层的新线程, 执行线程函数:threadFunc

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex>lock(mutex_);
        while (loop_ == nullptr)
        {
            cond_.wait(lock);
        }
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::setCpuAffinity(int cpu_id) 
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set); // 清空CPU集合
    CPU_SET(cpu_id, &cpu_set); // 将特定的CPU核心加入集合

    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
    if (result != 0) {
        std::cerr << "Failed to set CPU affinity: " << result << std::endl;
    } else {
        std::cout << "Thread bound to CPU " << cpu_id << std::endl;
    }
}


void EventLoopThread::threadFunc()
{
    EventLoop loop; // 创建一个独立的eventloop, one loop per thread

    if (callback_)
    {
        callback_(&loop); // 如果设置了相应的InitCallback, 先调用
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    loop.loop(); // EventLoop loop => Poller.poll 直到关闭服务器程序
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;

}