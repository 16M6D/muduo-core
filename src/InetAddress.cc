#include "InetAddress.h"

#include <strings.h>
#include <string.h>

// 参数默认值在定义和声明中只能出现一次
InetAddress::InetAddress(uint16_t port, std::string ip)
{
    bzero(&addr_, sizeof addr_);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = inet_addr(ip.c_str());
}

std::string InetAddress::toIP() const
{
    // addr_
    char buf[64] = {0};
    // 将网络地址从二进制格式转换为文本字符串格式
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    return buf; 
}

uint16_t InetAddress::toPort() const 
{
    return ntohs(addr_.sin_port);
}

std::string InetAddress::toIpPort() const
{
    // IP:Port
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    size_t end = strlen(buf);
    sprintf(buf + end, ":%u", ntohs(addr_.sin_port));
    return buf;
}

// #include<iostream>

// int main()
// {
//     InetAddress addr(8080);
//     std::cout << addr.toIpPort() << std::endl;
//     return 0;
// }