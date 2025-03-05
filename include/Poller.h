#pragma once

#include<vector>
#include<unordered_map>

#include "noncopyable.h"
#include "Channel.h"

// muduo库中多路事件分发器的核心IO复用模块
class Poller : noncopyable
{
public:
    using ChannelList = std::vector<Channel*>;
    
    Poller(EventLoop* loop);
    virtual ~Poller() = default;

    // 为所有IO复用，无论是epoll/poll/select，保留统一的接口以方便扩展
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    // 判断参数channel是否在此poller中
    bool hasChannel(Channel* channel) const;

    // 给EventLoop提供默认的IO复用接口
    static Poller* newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap channels_; // 为了派生类能够访问
private:
    EventLoop* ownerLoop_; //Poller所属的事件循环
};