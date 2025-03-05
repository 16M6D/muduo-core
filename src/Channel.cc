#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

#include <sys/epoll.h>

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; 
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
        : loop_(loop)
        , fd_(fd)
        , events_(0)
        , revents_(0)
        , index_(-1)
        , tied_(false)
{
}
Channel::~Channel()
{
}

// tie方法什么时候调用? 
// 一个TcpConnection新连接创建的时候 channel用一个weak_ptr保存其上层的TcpConnection对象的指针
// 当channel调用回调时,会先对该指针进行检查, 防止出现TcpConnection已经销毁但仍然调用回调的操作
void Channel::tie(const std::shared_ptr<void>&obj)
{
    tie_ = obj;
    tied_ = true;
}

// 负责改变fd在poller中的相应事件，但channel和poller本身是两个同级的类，需要通过所属的EventLoop来调用
void Channel::update()
{
    loop_->updateChannel(this);
}

void Channel::remove()
{
    loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_) 
    {
        std::shared_ptr<void>guard = tie_.lock();
        if (guard) {
            handleEventWithGuard(receiveTime);
        }
    }
    else
    {
        handleEventWithGuard(receiveTime);
    }
}

void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO("Channel handleEvent revents:%d\n", revents_);
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if (closeCallback_) closeCallback_();
    }
    if (revents_ & EPOLLERR)
    {
        if (errorCallback_) errorCallback_();
    }
    if (revents_ & (EPOLLIN | EPOLLPRI)) 
    {
        if (readCallback_) readCallback_(receiveTime);
    }
    if (revents_ & EPOLLOUT) 
    {
        if (writeCallback_) writeCallback_();
    }
}