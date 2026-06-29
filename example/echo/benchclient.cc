#include <mymuduo/EventLoop.h>
#include <mymuduo/Channel.h>
#include <mymuduo/InetAddress.h>
#include <mymuduo/Logger.h>
#include <mymuduo/Timestamp.h>
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
// 非阻塞 connect 的辅助函数
// ============================================================
static int createNonblockingSocket()
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0)
    {
        LOG_ERROR("createNonblockingSocket error: %s", strerror(errno));
    }
    return fd;
}

// ============================================================
// 压测参数
// ============================================================
struct BenchOptions
{
    std::string host = "127.0.0.1";
    uint16_t port = 8899;
    int connections = 1;
    int messages = 10000;
    int messageSize = 64;
    int duration = 0;
    int clientThreads = 1;
    bool verbose = false;
};

// ============================================================
// 单个 Echo 连接（ping-pong 模式）
// ============================================================
class PingPongClient
{
public:
    PingPongClient(EventLoop* loop, const InetAddress& serverAddr, int id)
        : loop_(loop)
        , serverAddr_(serverAddr)
        , id_(id)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
        , state_(kConnecting)
        , messagesSent_(0)
        , messagesRecv_(0)
        , bytesSent_(0)
        , bytesRecv_(0)
    {
        channel_->setWriteCallback(std::bind(&PingPongClient::handleWrite, this));
        channel_->setReadCallback(std::bind(&PingPongClient::handleRead, this, std::placeholders::_1));
        channel_->setCloseCallback(std::bind(&PingPongClient::handleClose, this));
        channel_->setErrorCallback(std::bind(&PingPongClient::handleError, this));

        payload_.assign(messageSize_, 'A' + (id % 26));
    }

    ~PingPongClient()
    {
        if (sockfd_ >= 0) ::close(sockfd_);
    }

    int id() const { return id_; }
    bool finished() const { return state_ == kDone; }

    void connect()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        int savedErrno = (ret == 0) ? 0 : errno;
        if (savedErrno == EINPROGRESS || savedErrno == 0)
        {
            state_ = kConnecting;
            channel_->enableWriting();
            channel_->enableReading();
        }
        else
        {
            LOG_ERROR("connect error: %s", strerror(savedErrno));
            state_ = kDone;
        }
    }

    void setNumMessages(int n) { totalMessages_ = n; }
    static void setMessageSize(int s) { messageSize_ = s; }

    // 延迟采样
    std::vector<double> latencies; // 微秒

private:
    enum State { kConnecting, kConnected, kDone };

    void handleWrite()
    {
        if (state_ == kConnecting)
        {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            {
                state_ = kConnected;
                int one = 1;
                ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                channel_->disableWriting();
                sendOne();
            }
            else
            {
                LOG_ERROR("PingPongClient[%d] connect failed: %s", id_, strerror(err));
                state_ = kDone;
            }
        }
        else if (state_ == kConnected)
        {
            channel_->disableWriting();
            sendOne();
        }
    }

    void handleRead(Timestamp receiveTime)
    {
        (void)receiveTime;
        int savedErrno = 0;
        ssize_t n = inputBuffer_.readFd(sockfd_, &savedErrno);
        if (n > 0)
        {
            bytesRecv_ += n;
            while (inputBuffer_.readableBytes() >= static_cast<size_t>(messageSize_))
            {
                inputBuffer_.retrieveAsString(messageSize_);
                messagesRecv_++;

                auto now = std::chrono::steady_clock::now();
                if (!sendTimes_.empty())
                {
                    auto& tp = sendTimes_.front();
                    double latUs = std::chrono::duration<double, std::micro>(
                        now - tp).count();
                    latencies.push_back(latUs);
                    sendTimes_.pop_front();
                }

                if (messagesRecv_ >= totalMessages_)
                {
                    state_ = kDone;
                    channel_->disableAll();
                    return;
                }
                // 服务器每次 echo 后会 shutDown，所以不在这里 sendOne
                // handleClose 会自动重连并发送下一个消息
            }
        }
        else if (n == 0)
        {
            handleClose();
        }
        else
        {
            LOG_ERROR("PingPongClient[%d] read error: %s", id_, strerror(savedErrno));
        }
    }

    void handleClose()
    {
        // 服务器主动关闭连接（一次 echo 后 shutDown）
        // 如果还有消息要发，自动重连
        if (messagesRecv_ < totalMessages_)
        {
            channel_->disableAll();
            channel_->remove();
            ::close(sockfd_);
            sockfd_ = createNonblockingSocket();
            delete channel_;
            channel_ = new Channel(loop_, sockfd_);
            channel_->setWriteCallback(std::bind(&PingPongClient::handleWrite, this));
            channel_->setReadCallback(std::bind(&PingPongClient::handleRead, this, std::placeholders::_1));
            channel_->setCloseCallback(std::bind(&PingPongClient::handleClose, this));
            channel_->setErrorCallback(std::bind(&PingPongClient::handleError, this));
            state_ = kConnecting;
            inputBuffer_.retrieveAll();

            int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                                sizeof(sockaddr_in));
            int savedErrno = (ret == 0) ? 0 : errno;
            if (savedErrno == EINPROGRESS || savedErrno == 0)
            {
                channel_->enableWriting();
                channel_->enableReading();
            }
            else
            {
                LOG_ERROR("PingPongClient[%d] reconnect error: %s", id_, strerror(savedErrno));
                state_ = kDone;
            }
        }
        else
        {
            state_ = kDone;
            channel_->disableAll();
        }
    }

    void handleError()
    {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len);
        LOG_ERROR("PingPongClient[%d] error: %s", id_, strerror(err));
        state_ = kDone;
    }

    void sendOne()
    {
        if (state_ != kConnected) return;
        if (messagesSent_ >= totalMessages_) return;

        sendTimes_.push_back(std::chrono::steady_clock::now());

        ssize_t n = ::write(sockfd_, payload_.data(), payload_.size());
        if (n > 0)
        {
            bytesSent_ += n;
            messagesSent_++;
        }
        else if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                channel_->enableWriting();
            }
            else
            {
                LOG_ERROR("PingPongClient[%d] write error: %s", id_, strerror(errno));
                state_ = kDone;
            }
        }
    }

public:
    int messagesSent() const { return messagesSent_; }
    int messagesRecv() const { return messagesRecv_; }
    int64_t bytesSent() const { return bytesSent_; }
    int64_t bytesRecv() const { return bytesRecv_; }

private:
    EventLoop* loop_;
    InetAddress serverAddr_;
    int id_;
    int sockfd_;
    Channel* channel_;
    State state_;
    Buffer inputBuffer_;
    std::string payload_;

    int totalMessages_ = 0;
    int messagesSent_;
    int messagesRecv_;
    int64_t bytesSent_;
    int64_t bytesRecv_;
    std::deque<std::chrono::steady_clock::time_point> sendTimes_;

    static int messageSize_;
};

int PingPongClient::messageSize_ = 64;

// ============================================================
// 统计工具
// ============================================================
static double percent(const std::vector<double>& data, double pct)
{
    if (data.empty()) return 0;
    size_t idx = static_cast<size_t>(data.size() * pct / 100.0);
    if (idx >= data.size()) idx = data.size() - 1;
    return data[idx];
}

static void printLatencyStats(const std::string& label, std::vector<double>& latencies,
                               double totalSec)
{
    if (latencies.empty()) return;
    std::sort(latencies.begin(), latencies.end());

    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avg = sum / latencies.size();
    double minVal = latencies.front();
    double maxVal = latencies.back();
    double p50 = percent(latencies, 50);
    double p90 = percent(latencies, 90);
    double p99 = percent(latencies, 99);
    double p999 = percent(latencies, 99.9);

    double sqSum = 0;
    for (double v : latencies) sqSum += (v - avg) * (v - avg);
    double stddev = std::sqrt(sqSum / latencies.size());

    printf("\n  %s 延迟统计 (总计 %zu 次, 耗时 %.3fs):\n", label.c_str(), latencies.size(), totalSec);
    printf("  ┌──────────┬─────────────┐\n");
    printf("  │ 指标     │ 延迟 (μs)   │\n");
    printf("  ├──────────┼─────────────┤\n");
    printf("  │ Min      │ %11.1f │\n", minVal);
    printf("  │ Avg      │ %11.1f │\n", avg);
    printf("  │ Max      │ %11.1f │\n", maxVal);
    printf("  │ Stddev   │ %11.1f │\n", stddev);
    printf("  │ P50      │ %11.1f │\n", p50);
    printf("  │ P90      │ %11.1f │\n", p90);
    printf("  │ P99      │ %11.1f │\n", p99);
    printf("  │ P99.9    │ %11.1f │\n", p999);
    printf("  ├──────────┼─────────────┤\n");
    printf("  │ QPS      │ %10.1f  │\n", latencies.size() / totalSec);
    printf("  └──────────┴─────────────┘\n");
}

// ============================================================
// 在单个线程中运行多个 ping-pong 客户端
// ============================================================
static void runPingPongInThread(int threadIdx, const InetAddress& serverAddr,
                                 int numConns, int numMsgs, int msgSize,
                                 std::vector<double>* outLatencies,
                                 std::atomic<int>* completedCount)
{
    PingPongClient::setMessageSize(msgSize);

    EventLoop loop;
    std::vector<std::unique_ptr<PingPongClient>> clients;

    for (int i = 0; i < numConns; i++)
    {
        int globalId = threadIdx * numConns + i;
        std::unique_ptr<PingPongClient> client(new PingPongClient(&loop, serverAddr, globalId));
        client->setNumMessages(numMsgs);
        client->connect();
        clients.push_back(std::move(client));
    }

    // 定时器，每 100ms 检查一次是否所有客户端都完成了
    int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel timerChannel(&loop, timerFd);
    timerChannel.setReadCallback([&](Timestamp) {
        uint64_t exp;
        ::read(timerFd, &exp, sizeof(exp));

        bool allDone = true;
        for (auto& c : clients)
        {
            if (!c->finished())
                allDone = false;
        }

        if (allDone)
        {
            loop.quit();
        }
    });
    timerChannel.enableReading();

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 100 * 1000 * 1000;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 100 * 1000 * 1000;
    ::timerfd_settime(timerFd, 0, &its, nullptr);

    auto startTime = std::chrono::steady_clock::now();
    loop.loop();
    auto endTime = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(endTime - startTime).count();

    // 收集延迟数据
    int totalSent = 0, totalRecv = 0;
    int64_t totalBytesSent = 0, totalBytesRecv = 0;
    for (auto& c : clients)
    {
        totalSent += c->messagesSent();
        totalRecv += c->messagesRecv();
        totalBytesSent += c->bytesSent();
        totalBytesRecv += c->bytesRecv();
        for (double lat : c->latencies)
            outLatencies->push_back(lat);
    }

    completedCount->fetch_add(totalRecv);

    printf("\n[线程 %d] 完成: %d 连接 × %d 消息/连接\n", threadIdx, numConns, numMsgs);
    printf("  发送: %d 条, 接收: %d 条, 发送字节: %ld, 接收字节: %ld\n",
           totalSent, totalRecv, totalBytesSent, totalBytesRecv);
    printLatencyStats("线程#" + std::to_string(threadIdx), *outLatencies, totalSec);
    printf("  吞吐量: %.2f MB/s\n",
           totalBytesRecv / totalSec / 1024.0 / 1024.0);

    ::close(timerFd);
}

// ============================================================
// 吞吐量模式 — 多连接持续发送
// ============================================================
class ThroughputClient
{
public:
    ThroughputClient(EventLoop* loop, const InetAddress& serverAddr, int id)
        : loop_(loop)
        , serverAddr_(serverAddr)
        , id_(id)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
        , state_(kConnecting)
        , bytesRecv_(0)
        , messagesRecv_(0)
    {
        channel_->setWriteCallback(std::bind(&ThroughputClient::handleWrite, this));
        channel_->setReadCallback(std::bind(&ThroughputClient::handleRead, this, std::placeholders::_1));
        channel_->setCloseCallback(std::bind(&ThroughputClient::handleClose, this));
        channel_->setErrorCallback(std::bind(&ThroughputClient::handleError, this));

        payload_.assign(1024, 'X');
    }

    ~ThroughputClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    bool connected() const { return state_ == kConnected; }
    int64_t bytesRecv() const { return bytesRecv_; }
    int messagesRecv() const { return messagesRecv_; }

    void connect()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        if (ret < 0 && errno != EINPROGRESS)
        {
            LOG_ERROR("ThroughputClient[%d] connect error: %s", id_, strerror(errno));
            state_ = kDone;
            return;
        }
        state_ = kConnecting;
        channel_->enableWriting();
        channel_->enableReading();
    }

    void disconnect()
    {
        if (state_ == kConnected)
        {
            ::shutdown(sockfd_, SHUT_WR);
            state_ = kDone;
        }
    }

private:
    enum State { kConnecting, kConnected, kDone };

    void handleWrite()
    {
        if (state_ == kConnecting)
        {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            {
                state_ = kConnected;
                int one = 1;
                ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                writeMore();
            }
            else
            {
                state_ = kDone;
            }
        }
        else if (state_ == kConnected)
        {
            writeMore();
        }
    }

    void writeMore()
    {
        while (true)
        {
            ssize_t n = ::write(sockfd_, payload_.data(), payload_.size());
            if (n > 0)
            {
                // 继续写
            }
            else if (n < 0)
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    break;
                else
                {
                    state_ = kDone;
                    break;
                }
            }
        }
    }

    void handleRead(Timestamp)
    {
        char buf[65536];
        while (true)
        {
            ssize_t n = ::read(sockfd_, buf, sizeof(buf));
            if (n > 0)
            {
                bytesRecv_ += n;
                messagesRecv_++;
            }
            else if (n == 0)
            {
                handleClose();
                break;
            }
            else
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    break;
                else
                    break;
            }
        }
    }

    void handleClose() { state_ = kDone; channel_->disableAll(); }
    void handleError() { state_ = kDone; }

    EventLoop* loop_;
    InetAddress serverAddr_;
    int id_;
    int sockfd_;
    Channel* channel_;
    State state_;
    std::string payload_;
    int64_t bytesRecv_;
    int messagesRecv_;
};

static void runThroughputInThread(int threadIdx, const InetAddress& serverAddr,
                                   int numConns, int durationSec,
                                   std::atomic<int64_t>* totalBytes,
                                   std::atomic<int>* completedCount)
{
    (void)threadIdx;
    EventLoop loop;
    std::vector<std::unique_ptr<ThroughputClient>> clients;

    for (int i = 0; i < numConns; i++)
    {
        std::unique_ptr<ThroughputClient> client(new ThroughputClient(&loop, serverAddr, i));
        client->connect();
        clients.push_back(std::move(client));
    }

    int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel timerChannel(&loop, timerFd);
    timerChannel.setReadCallback([&](Timestamp) {
        uint64_t exp;
        ::read(timerFd, &exp, sizeof(exp));
        for (auto& c : clients) c->disconnect();
        loop.quit();
    });
    timerChannel.enableReading();

    struct itimerspec its;
    its.it_value.tv_sec = durationSec;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    ::timerfd_settime(timerFd, 0, &its, nullptr);

    loop.loop();

    int64_t total = 0;
    int msgs = 0;
    for (auto& c : clients)
    {
        total += c->bytesRecv();
        msgs += c->messagesRecv();
    }
    totalBytes->fetch_add(total);
    completedCount->fetch_add(msgs);

    ::close(timerFd);
}

// ============================================================
// 并发连接数压力测试
// ============================================================
class ConnTestClient
{
public:
    ConnTestClient(EventLoop* loop, const InetAddress& serverAddr, int id,
                   std::atomic<int>* connectedCount)
        : loop_(loop)
        , serverAddr_(serverAddr)
        , id_(id)
        , sockfd_(createNonblockingSocket())
        , channel_(new Channel(loop, sockfd_))
        , connectedCount_(connectedCount)
    {
        channel_->setWriteCallback(std::bind(&ConnTestClient::handleWrite, this));
        channel_->setReadCallback(std::bind(&ConnTestClient::handleRead, this, std::placeholders::_1));
        channel_->setCloseCallback(std::bind(&ConnTestClient::handleClose, this));
        channel_->setErrorCallback(std::bind(&ConnTestClient::handleError, this));
    }

    ~ConnTestClient() { if (sockfd_ >= 0) ::close(sockfd_); }

    bool isConnected() const { return connected_; }
    bool isDone() const { return done_; }

    void connect()
    {
        int ret = ::connect(sockfd_, (const sockaddr*)serverAddr_.getSockAddr(),
                            sizeof(sockaddr_in));
        if (ret < 0 && errno != EINPROGRESS)
        {
            done_ = true;
            return;
        }
        channel_->enableWriting();
        channel_->enableReading();
    }

private:
    void handleWrite()
    {
        if (!connected_)
        {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            {
                connected_ = true;
                connectedCount_->fetch_add(1);
                channel_->disableWriting();
                int one = 1;
                ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            }
            else
            {
                done_ = true;
            }
        }
    }

    void handleRead(Timestamp)
    {
        char buf[64];
        ::read(sockfd_, buf, sizeof(buf));
    }

    void handleClose()
    {
        if (connected_) connectedCount_->fetch_sub(1);
        connected_ = false;
        done_ = true;
    }

    void handleError() { done_ = true; }

    EventLoop* loop_;
    InetAddress serverAddr_;
    int id_;
    int sockfd_;
    Channel* channel_;
    std::atomic<int>* connectedCount_;
    bool connected_ = false;
    bool done_ = false;
};

static void runConnTestInThread(int threadIdx, const InetAddress& serverAddr,
                                 int numConns,
                                 std::atomic<int>* connectedCount,
                                 std::atomic<int>* attemptedCount,
                                 std::atomic<int>* failedCount)
{
    (void)threadIdx;
    EventLoop loop;
    std::vector<std::unique_ptr<ConnTestClient>> clients;

    for (int i = 0; i < numConns; i++)
    {
        std::unique_ptr<ConnTestClient> client(new ConnTestClient(
            &loop, serverAddr, threadIdx * numConns + i, connectedCount));
        client->connect();
        clients.push_back(std::move(client));
        attemptedCount->fetch_add(1);
    }

    int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Channel timerChannel(&loop, timerFd);
    int elapsed = 0;
    timerChannel.setReadCallback([&](Timestamp) {
        uint64_t exp;
        ::read(timerFd, &exp, sizeof(exp));
        elapsed += 100;

        bool allDone = true;
        for (auto& c : clients)
        {
            if (!c->isDone() && !c->isConnected())
                allDone = false;
        }

        if (allDone || elapsed >= 10000)
        {
            for (auto& c : clients)
                if (!c->isConnected() && !c->isDone())
                    failedCount->fetch_add(1);
            loop.quit();
        }
    });
    timerChannel.enableReading();

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 100 * 1000 * 1000;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 100 * 1000 * 1000;
    ::timerfd_settime(timerFd, 0, &its, nullptr);

    loop.loop();
    ::close(timerFd);
}

// ============================================================
// 用法
// ============================================================
static void usage(const char* prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -h <host>       服务器地址 (默认: 127.0.0.1)\n");
    printf("  -p <port>       服务器端口 (默认: 8899)\n");
    printf("  -c <conns>      并发连接数 (默认: 1)\n");
    printf("  -n <msgs>       每连接消息数 (默认: 10000)\n");
    printf("  -s <size>       消息大小(bytes) (默认: 64)\n");
    printf("  -d <seconds>    测试时长 (秒), 与 -n 互斥\n");
    printf("  -t <threads>    客户端线程数 (默认: 1)\n");
    printf("  --mode <mode>   pingpong | throughput | conns (默认: pingpong)\n");
    printf("  -v              详细输出\n");
    printf("\n示例:\n");
    printf("  # 单连接 ping-pong 延迟测试\n");
    printf("  %s -c 1 -n 10000 -s 64\n", prog);
    printf("  # 100连接 × 1线程 ping-pong\n");
    printf("  %s -c 100 -n 1000\n", prog);
    printf("  # 吞吐量测试: 10连接, 持续 10秒\n");
    printf("  %s --mode throughput -c 10 -d 10\n", prog);
    printf("  # 并发连接数压测: 尝试建立 1000 个连接\n");
    printf("  %s --mode conns -c 1000\n", prog);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[])
{
    BenchOptions opts;

    enum { MODE_OPT = 256 };
    std::string mode = "pingpong";

    static struct option long_options[] = {
        {"mode", required_argument, 0, MODE_OPT},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:c:n:s:d:t:v", long_options, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'h': opts.host = optarg; break;
        case 'p': opts.port = static_cast<uint16_t>(atoi(optarg)); break;
        case 'c': opts.connections = atoi(optarg); break;
        case 'n': opts.messages = atoi(optarg); break;
        case 's': opts.messageSize = atoi(optarg); break;
        case 'd': opts.duration = atoi(optarg); break;
        case 't': opts.clientThreads = atoi(optarg); break;
        case 'v': opts.verbose = true; break;
        case MODE_OPT: mode = optarg; break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    InetAddress serverAddr(opts.port, opts.host);

    printf("============================================\n");
    printf("  muduo Echo 服务器压测工具\n");
    printf("============================================\n");
    printf("  目标: %s:%d\n", opts.host.c_str(), opts.port);
    printf("  模式: %s\n", mode.c_str());
    printf("============================================\n\n");

    auto totalStart = std::chrono::steady_clock::now();

    if (mode == "pingpong" || mode == "pp")
    {
        int connsPerThread = opts.connections / opts.clientThreads;
        int remainder = opts.connections % opts.clientThreads;

        std::vector<std::thread> threads;
        std::vector<std::vector<double>> latencyBuckets(opts.clientThreads);
        std::atomic<int> completedCount{0};

        for (int t = 0; t < opts.clientThreads; t++)
        {
            int nConns = connsPerThread + (t < remainder ? 1 : 0);

            threads.emplace_back([&, t, nConns]() {
                runPingPongInThread(t, serverAddr, nConns, opts.messages,
                                     opts.messageSize, &latencyBuckets[t],
                                     &completedCount);
            });
        }

        for (auto& th : threads) th.join();

        std::vector<double> allLatencies;
        for (auto& bucket : latencyBuckets)
            allLatencies.insert(allLatencies.end(), bucket.begin(), bucket.end());

        auto totalEnd = std::chrono::steady_clock::now();
        double totalSec = std::chrono::duration<double>(totalEnd - totalStart).count();

        printf("\n============================================\n");
        printf("  汇总结果\n");
        printf("============================================\n");
        printf("  总连接数: %d\n", opts.connections);
        printf("  总消息数: %d 发送 / %zu 接收\n",
               opts.connections * opts.messages, allLatencies.size());
        printf("  总耗时: %.3f 秒\n", totalSec);
        printLatencyStats("汇总", allLatencies, totalSec);
        printf("  平均 QPS: %.1f\n", allLatencies.size() / totalSec);
        printf("  吞吐量: %.2f MB/s\n",
               allLatencies.size() * opts.messageSize / totalSec / 1024.0 / 1024.0);
    }
    else if (mode == "throughput" || mode == "tp")
    {
        if (opts.duration == 0) opts.duration = 10;

        int connsPerThread = opts.connections / opts.clientThreads;
        int remainder = opts.connections % opts.clientThreads;

        std::vector<std::thread> threads;
        std::atomic<int64_t> totalBytes{0};
        std::atomic<int> totalMsgs{0};

        for (int t = 0; t < opts.clientThreads; t++)
        {
            int nConns = connsPerThread + (t < remainder ? 1 : 0);

            threads.emplace_back([&, t, nConns]() {
                runThroughputInThread(t, serverAddr, nConns, opts.duration,
                                       &totalBytes, &totalMsgs);
            });
        }

        printf("  测试运行中, 持续 %d 秒...\n", opts.duration);
        for (auto& th : threads) th.join();

        auto totalEnd = std::chrono::steady_clock::now();
        double totalSec = std::chrono::duration<double>(totalEnd - totalStart).count();

        printf("\n============================================\n");
        printf("  汇总结果\n");
        printf("============================================\n");
        printf("  连接数: %d, 时长: %d 秒\n", opts.connections, opts.duration);
        printf("  总接收: %ld 字节 (%.2f MB)\n",
               totalBytes.load(), totalBytes.load() / 1024.0 / 1024.0);
        printf("  总消息: %d\n", totalMsgs.load());
        printf("  吞吐量: %.2f MB/s\n",
               totalBytes.load() / totalSec / 1024.0 / 1024.0);
        printf("  消息率: %.1f msg/s\n", totalMsgs.load() / totalSec);
    }
    else if (mode == "conns")
    {
        int connsPerThread = opts.connections / opts.clientThreads;
        int remainder = opts.connections % opts.clientThreads;

        std::vector<std::thread> threads;
        std::atomic<int> connectedCount{0};
        std::atomic<int> attemptedCount{0};
        std::atomic<int> failedCount{0};

        auto startTime = std::chrono::steady_clock::now();

        for (int t = 0; t < opts.clientThreads; t++)
        {
            int nConns = connsPerThread + (t < remainder ? 1 : 0);

            threads.emplace_back([&, t, nConns]() {
                runConnTestInThread(t, serverAddr, nConns,
                                     &connectedCount, &attemptedCount, &failedCount);
            });
        }

        printf("  正在建立 %d 个连接...\n", opts.connections);
        for (auto& th : threads) th.join();

        auto endTime = std::chrono::steady_clock::now();
        double totalSec = std::chrono::duration<double>(endTime - startTime).count();

        printf("\n============================================\n");
        printf("  汇总结果\n");
        printf("============================================\n");
        printf("  尝试连接: %d\n", attemptedCount.load());
        printf("  成功连接: %d\n", connectedCount.load());
        printf("  失败连接: %d\n", failedCount.load());
        printf("  成功率: %.2f%%\n",
               100.0 * connectedCount.load() / (attemptedCount.load() ? attemptedCount.load() : 1));
        printf("  耗时: %.3f 秒\n", totalSec);
        printf("  连接速率: %.1f conn/s\n", connectedCount.load() / totalSec);
    }
    else
    {
        printf("未知模式: %s. 支持: pingpong, throughput, conns\n", mode.c_str());
        usage(argv[0]);
        return 1;
    }

    printf("\n完成.\n");
    return 0;
}
