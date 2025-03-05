#pragma once

/*
 * 作为基类被继承
 * 限制派生类对象的拷贝构造和赋值操作
 * 派生类对象可以正常构造和析构
 */
class noncopyable
{
public:
	noncopyable(const noncopyable&) = delete;
	void operator=(const noncopyable&) = delete;
protected:
	noncopyable() = default;
	~noncopyable() = default;
};
