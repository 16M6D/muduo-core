#include "Poller.h"

Poller::Poller(EventLoop* loop)
    : ownerLoop_(loop)
{
}

bool Poller::hasChannel(Channel* channel) const
{   
    // channels_是一个哈希表类型，存储的是 key:sockfd, value: 通道类型
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}