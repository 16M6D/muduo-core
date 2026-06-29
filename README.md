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