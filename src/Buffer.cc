#include "Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

/**
 * 从fd上读取数据
 */

ssize_t Buffer::readFd(int fd, int* saveErrno)
{
    char extrabuf[65536] = {0}; // memory in stack 64K
    struct iovec vec[2];
    
    const size_t writeable = writeableBytes();
    
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writeable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    const int iovcnt = (writeable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt); //读取数据
    if (n < 0)
    {
        *saveErrno = errno;
    }
    else if (n > writeable) 
    {   
        // extarbuf中写入数据, 需要扩容
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writeable); // 从writeIndex_开始写入n - writeable的数据
    }
    else 
    {
        // 缓冲区足够存储所读数据
        writerIndex_ += n;
    }
    return n;
}
ssize_t Buffer::writeFd(int fd, int* saveErrno)
{
    ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0)
    {
        *saveErrno = errno;
    }
    return n;
}