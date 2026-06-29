# muduo-core

## Intro

基于 muduo 网络库的 C++ 网络库项目，用 C++11 标准库替换了原版对 boost 的依赖，并增加了线程 CPU 亲和性绑定功能。

## 压测结果

WSL2 (Linux 5.15), echo server, keep-alive 长连接:

| 指标 | 结果 |
|------|------|
| P50 延迟 | 56 μs |
| P99 延迟 | 107 μs |
| QPS (单连接) | 17,000+ |
| 吞吐量 (10 连接) | 847 MB/s |
| 并发空闲连接 | 1,000 (100%, 9,700 conn/s) |
| 消息丢失 | 0 / 100,000 |
| 内存泄漏 | 0 (valgrind: definitely lost = 0 bytes) |

压测工具: `example/echo/benchclient.cc`, 支持 latency / throughput / conns 三种模式。

```bash
./benchclient --mode latency    -n 100000 -s 64    # 延迟测试
./benchclient --mode throughput -c 10 -d 10         # 吞吐量测试
./benchclient --mode conns      -c 1000             # 并发连接测试
```

## Main Task

### 自动线程数设置

- `setThreadNum(0)` 时自动匹配 CPU 核心数。
- 默认支持 CPU 亲和性绑定。
- 在多核机器上建议 `setThreadNum(0)` + 绑定核心。

### 为什么 CPU 亲和性解决了消息丢问题

**根因是线程迁移导致 epoll 事件处理不及时。**

muduo 的架构是 one loop per thread：每个 subloop 线程拥有独立的 epoll 实例，
通过 `getNextLoop()` 轮询分配连接。没有 CPU 亲和性时：

1. **线程被 OS 调度器迁移到其他核心** → L1/L2 缓存全部失效（cold cache），
   线程需要重新加载 epoll 事件队列、活跃 channel 列表等热数据

2. **迁移间隙线程暂停执行** → `epoll_wait()` 不被调用 → 对应 fd 的 TCP
   接收缓冲区数据无人消费 → 内核缓冲区满 → **TCP 通告零窗口 → 对端停止发送
   → 超时后数据丢失**

3. **wakeupFd 跨核延迟** — muduo 通过 eventfd 唤醒其他线程执行回调（如
   `runInLoop`/`queueInLoop`），线程迁移会放大这个唤醒延迟，send 等关键操作
   无法及时执行

绑定 CPU 核心后：每个 subloop 线程独占一个核心 → L1/L2 缓存常驻热数据 → epoll
轮询延迟稳定在微秒级 → 内核缓冲区始终被及时消费 → 不再丢消息。这正是压测中
100,000 条请求零丢失的原因。
