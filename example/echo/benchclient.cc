// ================================================================
//  muduo Echo Server 压测工具 (v2)
//
//  三种测试模式:
//    latency    — 单长连接 ping-pong，测纯事件循环延迟
//    throughput — N 长连接持续收发，测极限吞吐量
//    conns      — N 空闲连接，测并发连接上限
//
//  用法: ./benchclient [--mode latency|throughput|conns] [选项]
// ================================================================

#include <mymuduo/EventLoop.h>
#include <mymuduo/Channel.h>
#include <mymuduo/InetAddress.h>
#include <mymuduo/Logger.h>
#include <mymuduo/Buffer.h>

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <atomic>
#include <vector>
#include <deque>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
#include <getopt.h>

// ============================================================
// 工具函数
// ============================================================
static int createNonblockingSocket()
{
    return ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
}

static double nowUs()
{
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return ns / 1000.0;
}

static double pct(std::vector<double>& data, double p)
{
    if (data.empty()) return 0;
    size_t idx = static_cast<size_t>(data.size() * p / 100.0);
    if (idx >= data.size()) idx = data.size() - 1;
    return data[idx];
}

static void printLatency(const std::string& label, std::vector<double>& lat,
                          double elapsedSec, int64_t totalBytes)
{
    if (lat.empty()) { printf("\n  (无数据)\n"); return; }
    std::sort(lat.begin(), lat.end());
    double sum = std::accumulate(lat.begin(), lat.end(), 0.0);
    double avg = sum / lat.size();

    double sq = 0;
    for (double v : lat) sq += (v - avg) * (v - avg);

    printf("\n  %s\n", label.c_str());
    printf("  ┌──────────┬────────────────┐\n");
    printf("  │ 指标     │ 值             │\n");
    printf("  ├──────────┼────────────────┤\n");
    printf("  │ 请求数   │ %14zu │\n", lat.size());
    printf("  │ 耗时     │ %13.3f s │\n", elapsedSec);
    printf("  │ Min      │ %13.1f μs │\n", lat.front());
    printf("  │ Avg      │ %13.1f μs │\n", avg);
    printf("  │ P50      │ %13.1f μs │\n", pct(lat, 50));
    printf("  │ P90      │ %13.1f μs │\n", pct(lat, 90));
    printf("  │ P99      │ %13.1f μs │\n", pct(lat, 99));
    printf("  │ P99.9    │ %13.1f μs │\n", pct(lat, 99.9));
    printf("  │ P99.99   │ %13.1f μs │\n", pct(lat, 99.99));
    printf("  │ Max      │ %13.1f μs │\n", lat.back());
    printf("  │ Stddev   │ %13.1f μs │\n", std::sqrt(sq / lat.size()));
    printf("  │ QPS      │ %13.1f    │\n", lat.size() / elapsedSec);
    printf("  │ 吞吐量   │ %11.2f MB/s │\n",
           totalBytes / elapsedSec / 1024.0 / 1024.0);
    printf("  └──────────┴────────────────┘\n");
}

// ============================================================
// 模式 1: LatencyClient — 单长连接 ping-pong
//
// 协议: 每次 send() 一个完整消息, 等待收到同样大小的回显,
//       记录 RTT, 然后发送下一条。
//       连接全程保持, 不重连。
// ============================================================
class LatencyClient
{
public:
    LatencyClient(EventLoop* loop, const InetAddress& addr)
        : loop_(loop), serverAddr_(addr)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
    {
        channel_->setWriteCallback([this]() { onWrite(); });
        channel_->setReadCallback([this](Timestamp) { onRead(); });
        channel_->setCloseCallback([this]() { onClose(); });
        channel_->setErrorCallback([this]() { onError(); });
    }

    ~LatencyClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    // 设置参数
    void setMessage(int bytes)  { msg_.assign(bytes, 'A'); }
    void setCount(int n)        { total_ = n; }

    // 注入外部统计容器
    std::vector<double> latencies;
    int64_t bytesRecv = 0;

    int done()   const { return finished_ >= total_; }
    int sent()   const { return sent_; }
    int recved() const { return finished_; }

    void start()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        if (ret < 0 && errno != EINPROGRESS)
        {
            LOG_ERROR("LatencyClient connect error: %s", strerror(errno));
            finished_ = total_; // 标记完成（失败）
            return;
        }
        state_ = CONNECTING;
        channel_->enableWriting();
        channel_->enableReading();
        connectTime_ = nowUs();
    }

private:
    enum State { CONNECTING, CONNECTED, DONE };

    void onWrite()
    {
        if (state_ == CONNECTING)
        {
            int err = 0; socklen_t len = sizeof(err);
            if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            {
                state_ = CONNECTED;
                int one = 1;
                ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                channel_->disableWriting();
                // 连接建立, 开始发送第一条消息
                sendOne();
            }
            else
            {
                LOG_ERROR("LatencyClient connect failed: %s", strerror(err));
                finished_ = total_;
            }
        }
        else if (state_ == CONNECTED)
        {
            channel_->disableWriting();
            sendOne();
        }
    }

    void onRead()
    {
        int savedErrno = 0;
        ssize_t n = buf_.readFd(sockfd_, &savedErrno);
        if (n > 0)
        {
            bytesRecv += n;
            // 收到回显数据，尝试提取完整消息
            while (buf_.readableBytes() >= msg_.size())
            {
                buf_.retrieve(msg_.size());
                finished_++;
                double rtt = nowUs() - sendTime_;
                latencies.push_back(rtt);

                if (finished_ >= total_)
                {
                    state_ = DONE;
                    channel_->disableAll();
                    return;
                }
                // 发下一条
                sendOne();
            }
        }
        else if (n == 0)
        {
            LOG_ERROR("LatencyClient: server closed connection unexpectedly");
            finished_ = total_;
        }
    }

    void sendOne()
    {
        sendTime_ = nowUs();
        ssize_t n = ::write(sockfd_, msg_.data(), msg_.size());
        if (n > 0)
        {
            sent_++;
        }
        else if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
        {
            channel_->enableWriting(); // 等下次可写
        }
    }

    void onClose()  { finished_ = total_; }
    void onError()  { finished_ = total_; }

    EventLoop* loop_;
    InetAddress serverAddr_;
    int sockfd_;
    Channel* channel_;
    State state_ = CONNECTING;
    Buffer buf_;
    std::string msg_;

    int total_    = 0;
    int sent_     = 0;
    int finished_ = 0;

    double sendTime_    = 0;
    double connectTime_ = 0;
};

// ============================================================
// 模式 2: ThroughputClient — 持续收发
//
// 每个连接: 持续向 socket 写数据(不等回显), 同时持续读取回显。
//           服务器 echo 回来的所有数据都计入接收量。
// ============================================================
class ThroughputClient
{
public:
    ThroughputClient(EventLoop* loop, const InetAddress& addr, int id)
        : loop_(loop), serverAddr_(addr), id_(id)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
    {
        channel_->setWriteCallback([this]() { onWrite(); });
        channel_->setReadCallback([this](Timestamp) { onRead(); });
        channel_->setCloseCallback([this]() { onClose(); });
        channel_->setErrorCallback([this]() { onError(); });

        // 固定 1KB 负载
        payload_.assign(1024, 'B' + (id % 25));
    }

    ~ThroughputClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    int64_t bytesSent = 0;
    int64_t bytesRecv = 0;

    void start()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        if (ret < 0 && errno != EINPROGRESS)
        {
            failed_ = true;
            return;
        }
        state_ = CONNECTING;
        channel_->enableWriting();
        channel_->enableReading();
    }

    bool failed() const { return failed_; }

    void stop()
    {
        if (state_ == CONNECTED)
        {
            ::shutdown(sockfd_, SHUT_WR);
        }
        state_ = DONE;
        channel_->disableAll();
    }

private:
    enum State { CONNECTING, CONNECTED, DONE };

    void onWrite()
    {
        if (state_ == CONNECTING)
        {
            int err = 0; socklen_t len = sizeof(err);
            if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            {
                state_ = CONNECTED;
                int one = 1;
                ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            }
            else { failed_ = true; return; }
        }

        if (state_ != CONNECTED) return;

        // 持续写入直到 socket 缓冲区满
        while (true)
        {
            ssize_t n = ::write(sockfd_, payload_.data(), payload_.size());
            if (n > 0) bytesSent += n;
            else if (n < 0)
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN) return;
                else { failed_ = true; return; }
            }
        }
    }

    void onRead()
    {
        char buf[65536];
        while (true)
        {
            ssize_t n = ::read(sockfd_, buf, sizeof(buf));
            if (n > 0) bytesRecv += n;
            else if (n == 0) { onClose(); return; }
            else
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN) return;
                else { failed_ = true; return; }
            }
        }
    }

    void onClose() { state_ = DONE; }
    void onError() { failed_ = true; }

    EventLoop* loop_;
    InetAddress serverAddr_;
    int id_;
    int sockfd_;
    Channel* channel_;
    State state_ = CONNECTING;
    std::string payload_;
    bool failed_ = false;
};

// ============================================================
// 模式 3: 并发连接数测试
// ============================================================
class ConnTestClient
{
public:
    ConnTestClient(EventLoop* loop, const InetAddress& addr,
                   std::atomic<int>* counter)
        : loop_(loop), serverAddr_(addr)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
        , connCounter_(counter)
    {
        channel_->setWriteCallback([this]() { onWrite(); });
        channel_->setReadCallback([this](Timestamp) { onRead(); });
        channel_->setCloseCallback([this]() { onClose(); });
        channel_->setErrorCallback([this]() { onError(); });
    }

    ~ConnTestClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    bool isDone()     const { return done_; }
    bool isConnected() const { return connected_; }

    void start()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        if (ret < 0 && errno != EINPROGRESS) { done_ = true; return; }
        channel_->enableWriting();
        channel_->enableReading();
    }

private:
    void onWrite()
    {
        if (connected_) return;
        int err = 0; socklen_t len = sizeof(err);
        if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
        {
            connected_ = true;
            connCounter_->fetch_add(1);
            channel_->disableWriting();
            int one = 1;
            ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        }
        else { done_ = true; }
    }

    void onRead()
    {
        char buf[64];
        ::read(sockfd_, buf, sizeof(buf)); // discard
    }

    void onClose()
    {
        if (connected_) connCounter_->fetch_sub(1);
        connected_ = false;
        done_ = true;
    }
    void onError() { done_ = true; }

    EventLoop* loop_;
    InetAddress serverAddr_;
    int sockfd_;
    Channel* channel_;
    std::atomic<int>* connCounter_;
    bool connected_ = false;
    bool done_ = false;
};

// ============================================================
// 运行函数
// ============================================================

// --- Latency 模式 ---
static void runLatency(const InetAddress& addr, int msgSize, int count)
{
    EventLoop loop;

    LatencyClient client(&loop, addr);
    client.setMessage(msgSize);
    client.setCount(count);

    // 定时器检查完成状态
    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel tch(&loop, tfd);
    tch.setReadCallback([&](Timestamp) {
        uint64_t exp; ::read(tfd, &exp, sizeof(exp));
        if (client.done()) loop.quit();
    });
    tch.enableReading();

    struct itimerspec its = {};
    its.it_value.tv_nsec  = 50 * 1000 * 1000; // 50ms
    its.it_interval.tv_nsec = 50 * 1000 * 1000;
    ::timerfd_settime(tfd, 0, &its, nullptr);

    auto t0 = std::chrono::steady_clock::now();
    client.start();
    loop.loop();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    printf("\n  Latency 测试结果");
    printf("\n  ───────────────────────────");
    printf("\n  消息大小: %d bytes", msgSize);
    printf("\n  发送: %d, 接收: %d, 丢失: %d",
           client.sent(), client.recved(), client.sent() - client.recved());
    printLatency("RTT 分布", client.latencies, sec, client.bytesRecv);

    ::close(tfd);
}

// --- Throughput 模式 ---
static void runThroughput(const InetAddress& addr, int conns, int durationSec)
{
    EventLoop loop;
    std::vector<std::unique_ptr<ThroughputClient>> clients;

    for (int i = 0; i < conns; i++)
    {
        auto c = std::unique_ptr<ThroughputClient>(
            new ThroughputClient(&loop, addr, i));
        c->start();
        clients.push_back(std::move(c));
    }

    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel tch(&loop, tfd);
    tch.setReadCallback([&](Timestamp) {
        uint64_t exp; ::read(tfd, &exp, sizeof(exp));
        for (auto& c : clients) c->stop();
        loop.quit();
    });
    tch.enableReading();

    struct itimerspec its = {};
    its.it_value.tv_sec = durationSec;
    ::timerfd_settime(tfd, 0, &its, nullptr);

    printf("\n  测试运行中, 持续 %d 秒...\n", durationSec);

    auto t0 = std::chrono::steady_clock::now();
    loop.loop();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    int64_t totalSend = 0, totalRecv = 0;
    int failed = 0;
    for (auto& c : clients)
    {
        failed += c->failed() ? 1 : 0;
        totalSend += c->bytesSent;
        totalRecv += c->bytesRecv;
    }

    printf("\n  Throughput 测试结果");
    printf("\n  ───────────────────────────");
    printf("\n  连接数:  %d (成功: %d, 失败: %d)", conns, conns - failed, failed);
    printf("\n  时长:    %.3f s", sec);
    printf("\n  发送:    %ld bytes (%.2f MB)", totalSend, totalSend / 1024.0 / 1024.0);
    printf("\n  接收:    %ld bytes (%.2f MB)", totalRecv, totalRecv / 1024.0 / 1024.0);
    printf("\n  吞吐量:  %.2f MB/s", totalRecv / sec / 1024.0 / 1024.0);
    printf("\n  消息率:  %.1f K msg/s", totalRecv / sec / 1024.0 / 1000.0);
    printf("\n");

    ::close(tfd);
}

// --- Conns 模式 ---
static void runConns(const InetAddress& addr, int total)
{
    EventLoop loop;
    std::vector<std::unique_ptr<ConnTestClient>> clients;
    std::atomic<int> connectedCount{0};
    std::atomic<int> failedCount{0};

    for (int i = 0; i < total; i++)
    {
        auto c = std::unique_ptr<ConnTestClient>(
            new ConnTestClient(&loop, addr, &connectedCount));
        c->start();
        clients.push_back(std::move(c));
    }

    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel tch(&loop, tfd);
    int elapsed = 0;
    tch.setReadCallback([&](Timestamp) {
        uint64_t exp; ::read(tfd, &exp, sizeof(exp));
        elapsed += 100;
        bool allDone = true;
        for (auto& c : clients)
            if (!c->isDone() && !c->isConnected()) allDone = false;

        if (allDone || elapsed >= 30000)
        {
            for (auto& c : clients)
                if (!c->isConnected() && !c->isDone()) failedCount++;
            loop.quit();
        }
    });
    tch.enableReading();

    struct itimerspec its = {};
    its.it_value.tv_nsec  = 100 * 1000 * 1000;
    its.it_interval.tv_nsec = 100 * 1000 * 1000;
    ::timerfd_settime(tfd, 0, &its, nullptr);

    printf("\n  正在建立 %d 个连接...\n", total);

    auto t0 = std::chrono::steady_clock::now();
    loop.loop();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    printf("\n  Conns 测试结果");
    printf("\n  ───────────────────────────");
    printf("\n  尝试: %d", total);
    printf("\n  成功: %d", connectedCount.load());
    printf("\n  失败: %d", failedCount.load());
    printf("\n  成功率: %.2f%%", 100.0 * connectedCount / (total ?: 1));
    printf("\n  耗时: %.3f s", sec);
    printf("\n  连接速率: %.1f conn/s", connectedCount / sec);
    printf("\n");

    ::close(tfd);
}

// ============================================================
// 用法 & main
// ============================================================
static void usage(const char* prog)
{
    printf("用法: %s [--mode MODE] [选项]\n\n", prog);
    printf("模式 (--mode):\n");
    printf("  latency    单长连接 ping-pong 延迟测试 (默认)\n");
    printf("  throughput 多长连接持续收发吞吐量测试\n");
    printf("  conns      并发空闲连接数测试\n\n");
    printf("选项:\n");
    printf("  -p <port>   服务器端口 (默认: 8000)\n");
    printf("  -h <host>   服务器地址 (默认: 127.0.0.1)\n");
    printf("  -c <n>      连接数 (默认: 1, throughput 默认: 10)\n");
    printf("  -n <n>      请求总数 (latency 模式, 默认: 100000)\n");
    printf("  -s <bytes>  消息大小 (latency 模式, 默认: 64)\n");
    printf("  -d <sec>    测试时长 (throughput 模式, 默认: 10)\n\n");
    printf("示例:\n");
    printf("  %s -p 8000 -n 100000 -s 64            # 10万次 ping-pong 延迟测试\n", prog);
    printf("  %s --mode latency -s 4096 -n 10000    # 4KB 大消息延迟测试\n", prog);
    printf("  %s --mode throughput -c 10 -d 10      # 10连接 10秒吞吐量测试\n", prog);
    printf("  %s --mode conns -c 1000               # 1000并发连接测试\n", prog);
}

int main(int argc, char* argv[])
{
    std::string mode   = "latency";
    std::string host   = "127.0.0.1";
    uint16_t   port    = 8000;
    int        conns   = 1;
    int        count   = 100000;
    int        msgSize = 64;
    int        durSec  = 10;

    enum { OPT_MODE = 256 };
    static struct option longOpts[] = {
        {"mode", required_argument, 0, OPT_MODE},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:c:n:s:d:", longOpts, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'h': host    = optarg; break;
        case 'p': port    = static_cast<uint16_t>(atoi(optarg)); break;
        case 'c': conns   = atoi(optarg); break;
        case 'n': count   = atoi(optarg); break;
        case 's': msgSize = atoi(optarg); break;
        case 'd': durSec  = atoi(optarg); break;
        case OPT_MODE: mode = optarg; break;
        default: usage(argv[0]); return 1;
        }
    }

    InetAddress addr(port, host);

    printf("============================================\n");
    printf("  muduo Echo 压测工具 v2\n");
    printf("============================================\n");
    printf("  目标: %s:%d  模式: %s\n", host.c_str(), port, mode.c_str());
    printf("============================================\n");

    if (mode == "latency" || mode == "lat")
    {
        runLatency(addr, msgSize, count);
    }
    else if (mode == "throughput" || mode == "tp")
    {
        if (conns == 1) conns = 10; // throughput 默认 10 连接
        runThroughput(addr, conns, durSec);
    }
    else if (mode == "conns")
    {
        runConns(addr, conns);
    }
    else
    {
        printf("未知模式: %s\n", mode.c_str());
        usage(argv[0]);
        return 1;
    }

    printf("\n完成.\n");
    return 0;
}
