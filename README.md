# muduo-core

## Main Task
### 自动线程数设置
- `setThreadNum(0)` 时，自动匹配 CPU 核心数。
- 默认支持 CPU 亲和性绑定.
- 在多核机器上，建议 `setThreadNum(0)` + 绑定核心。