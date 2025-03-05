#pragma once

#include <vector>
#include <string>
#include <algorithm>

// 网络库底层的缓冲区类型定义
// @code
//  prependable bytes    |     readable bytes(CONTENT)          |  writeable bytes
// 0                   readIndex                          writeIndex                   size 
// @endcode
class Buffer
{
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitSize = 1024;

    explicit Buffer(size_t initialSize = kInitSize) 
        : buffer_(kCheapPrepend + kInitSize)
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {}
    ~Buffer() {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writeableBytes() const { return  buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; } 

    // 返回缓冲区中可读数据的起始位置
    const char* peek() const 
    {
        return begin() + readerIndex_;
    }

    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 应用只读取了缓冲区部分数据, 长度为len
        }
        else
        {
            retrieveAll();
        }
    }
    void retrieveAll()
    {
        readerIndex_ = writerIndex_  = kCheapPrepend; // 读后复位
    }

    // 把onMessage上报的Buffer数据转成string数据返回
    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes()); // 可读数据长度
    }
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 读取缓冲区中可读数据后, 对缓冲区进行复位操作
        return result;
    }

    // 需要确保剩余的缓冲区大小足够写下数据
    void ensureWriteableBytes(size_t len)
    {
        if (writeableBytes() < len)
        {
            makeSpace(len); // 扩容
        }
    }

    // 向缓冲区添加数据 [data, data + len] 
    void append(const char* data, size_t len)
    {
        ensureWriteableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    // 从fd上读取数据
    ssize_t readFd(int fd, int* saveErrno);
    // 通过fd发送数据
    ssize_t writeFd(int fd, int* saveErrno);

private:
    char* beginWrite()
    {
        return begin() + writerIndex_;
    }
    
    const char* beginWrite() const
    {
        return begin() + writerIndex_;
    }

    void makeSpace(size_t len)
    {
        // 可写的数据 + 已经读过的数据 都还是比len小, 只好发生扩容操作
        if (prependableBytes() + writeableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else // 整理碎片
        {
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }

    }

    char* begin()
    {
        return &*buffer_.begin(); // 获取容器首元素的地址,而非迭代器
    }
    const char* begin() const 
    {
        return &*buffer_.begin(); // 提供常方法
    }

    std::vector<char>buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};