#pragma once

#include<string>
#include<iostream>

#include "noncopyable.h"

//LOG_INFO("%s %d", arg1, arg2)
#define LOG_INFO(logmsgFormat,...) \
	do \
	{ \
		Logger &logger = Logger::instance(); \
		logger.setLogLevel(INFO); \
		char buf[1024] = {0}; \
		snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
		logger.log(buf); \
	} while(0)

//LOG_ERROR("%s %d", arg1, arg2)
#define LOG_ERROR(logmsgFormat,...) \
	do \
	{ \
		Logger &logger = Logger::instance(); \
		logger.setLogLevel(ERROR); \
		char buf[1024] = {0}; \
		snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
		logger.log(buf); \
	} while(0)

//LOG_FATAL("%s %d", arg1, arg2)
#define LOG_FATAL(logmsgFormat,...) \
	do \
	{ \
		Logger &logger = Logger::instance(); \
		logger.setLogLevel(FATAL); \
		char buf[1024] = {0}; \
		snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
		logger.log(buf); \
		exit(-1); \
	} while(0)

//LOG_DEBUG("%s %d", arg1, arg2)
#ifdef MUDEBUG
#define LOG_DEBUG(logmsgFormat,...) \
	do \
	{ \
		Logger &logger = Logger::instance(); \
		logger.setLogLevel(DEBUG); \
		char buf[1024] = {0}; \
		snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
		logger.log(buf); \
	} while(0)
#else
	#define LOG_DEBUG(logmsgFormat, ...)
#endif

// 定义日志级别
// INFO:  普通信息   	
// ERROR: 错误信息
// FATAL: core信息，系统无法继续执行
// DEBUG: 调试信息
enum LogLevel
{
	INFO,
	ERROR,
	FATAL,
	DEBUG
};

// 输出一个日志类
class Logger : noncopyable
{
public:
	// 获取日志唯一的实例对象
	static Logger& instance();
	// 设置日志级别
	void setLogLevel(int level);
	// 记录日志
	void log(std::string msg);
private:
	int logLevel_; // 下横杠放在后面以免与系统变量冲突
	Logger(){}     // 隐藏Logger的构造函数
};
