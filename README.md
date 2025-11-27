# muduo-core
## Intro
这是一个基于muduo网络库的C++网络库项目, 主要工作是通过C++11之后标准的标准库替换原有的基于boost库的实现; 另外, 在此基础上增加了线程与CPU的绑定功能, 利用CPU的多级缓存及局部性原理提升网络库整体表现.

## Main Task
### 自动线程数设置
- `setThreadNum(0)` 时，自动匹配 CPU 核心数。
- 默认支持 CPU 亲和性绑定.
- 在多核机器上，建议 `setThreadNum(0)` + 绑定核心
