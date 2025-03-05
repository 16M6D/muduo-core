#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <memory>

const int kNew = -1; // channel的成员index = -1，对应该channel没有添加到poller中
const int kAdded = 1;
const int kDeleted = 2;

EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop)
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC))
    , events_(kInitEventListSize)
    {
        if (epollfd_ < 0)
        {
            LOG_FATAL("epoll_create error:%d \n", errno);
        }
    }

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
    // poll的操作很频繁，应该用LOG_DEBUG
    LOG_INFO("func=%s => fd count=%lu\n", __FUNCTION__, channels_.size());

    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);  
    //小技巧：&*iterator
    int saveErrno = errno;
    Timestamp now(Timestamp::now());
    // 这也说明了muduo采用的是LT模式
    if (numEvents > 0)
    {
        LOG_INFO("%d events happened.\n", numEvents);
        fillActiveChannels(numEvents, activeChannels);
        // numsEvents最大就是把events_对应的vector填满而已，可能会发生扩容操作
        if (numEvents == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG("nothing happened.%s just timeout.\n", __FUNCTION__);
    }
    else
    {
        // 错误并非系统中断引起的
        if (saveErrno != EINTR)
        {
            errno = saveErrno; // 其他线程可能改变了全局的errno，导致并非原来的错误
            LOG_ERROR("EpollPoller::poll() error.\n");
        }
    }
    
    return now;
}

void EPollPoller::updateChannel(Channel* channel) 
{
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d events=%d index= %d \n", __FUNCTION__, channel->fd(), channel->events(), index);

    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else // channel already register 
    {
        int fd = channel->fd();
        if (channel->isNoneEvent())
        {
            channel->set_index(kDeleted);
            update(EPOLL_CTL_DEL, channel);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
void EPollPoller::removeChannel(Channel* channel)
{
    LOG_INFO("func=%s => fd=%d \n",__FUNCTION__, channel->fd());
    int fd = channel->fd();
    channels_.erase(fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

// 将有事件发生的Channel上报给EventLoop处理
void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const
{
    for (int i = 0; i < numEvents; ++i)
    {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

// epoll_ctl add/mod/del 的具体操作
void EPollPoller::update(int operation, Channel* channel)
{
    epoll_event event; // 通过epoll_ctl注册到内核
    memset(&event, 0, sizeof event);
    int fd = channel->fd();
    event.data.fd = fd;
    event.data.ptr = channel;
    event.events = channel->events();

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del failed:%d", errno);
        }
        else
        {
            LOG_FATAL("epoll_ctl add/mod failed:%d", errno);
        }
    }

}