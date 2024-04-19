#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <ostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include "config.h"

template<class F>
class DeferClass {
public:
    DeferClass(F&& f): m_func(std::forward<F>(f)) {}
    DeferClass(const F& f): m_func(f) {}
    ~DeferClass() {m_func();}

    DeferClass(const DeferClass& e) = delete;
    DeferClass& operator= (const DeferClass& rhs) = delete;
private:
    F m_func;
};

#define _CONCAT(a, b) a##b 
#define _MAKE_DEFER(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER(__LINE__)

void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

template<typename ...Args>
std::string format(const char* format_str, Args... args) {
    std::stringstream ss;
    int _[] = {((ss << args), 0)...};
    (void)_;
    return ss.str();
}

std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout();
void sleepNMilliseconds(int N);

// 异步写日志的日志队列，读也是阻塞的，和go channel一样
template<typename T>
class LockQueue {
public:
    // 多个工作线程写日志queue
    void Push(const T& data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(data);
        m_condvariable.notify_one();
    }

    // 一个线程读日志queue，写日志文件
    T pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_queue.empty()) {
            m_condvariable.wait(lock);
        }
        T data = m_queue.front();
        m_queue.pop();
        return data;
    }

    bool timeoutPop(int timeout, T* ResData) {
        std::unique_lock<std::mutex> lock(m_mutex);
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        auto timeout_time = now + std::chrono::milliseconds(timeout);

        while (m_queue.empty()) {
            if (m_condvariable.wait_until(lock, timeout_time) == std::cv_status::timeout) {
                return false;
            } else {
                continue;
            }
        }

        T data = m_queue.front();
        m_queue.pop();
        *ResData = data;
        return true;
    }
private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condvariable;
};



// 作为rpc调用参数传递给raft的操作对象
class Op {
public:
    // 
    std::string operation;
    // 插入键
    std::string key;
    // 插入值
    std::string value;
    // 请求的客户端ID标识
    std::string clientId;
    // 该客户端下请求的序列号（类似TCP序列号)，目的是保持线性一致性
    int requestId;
public:
    // 命令对象的简单序列化
    std::string asString() const {
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);

        oa << *this;
        return ss.str();
    }
    // 命令对象的简单反序列化
    bool parseFromString(const std::string& str) {
        std::stringstream iss(str);
        boost::archive::text_iarchive ia(iss);

        ia >> *this;
        return true;
    }
public:
    // 重载输出运算符
    friend std::ostream& operator<<(std::ostream& os, const Op& obj) {
        os << "[MyClass:Operation{" + obj.operation + "},Key{" + obj.key + "},Value{" + obj.value + "},ClientId{" +
              obj.clientId + "},RequestId{" + std::to_string(obj.requestId) + "}";
        return os;
    }
private:
    friend class boost::serialization::access;
    template<typename Archive>
    void serialize(Archive& ar, const unsigned int version) {
        ar& operation;
        ar& key;
        ar& value;
        ar& clientId;
        ar& requestId;
    }
};

// kvserver reply error
const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";

// 获取可用端口

// 判断端口是否被占用
bool isReleasePort(unsigned short usPort);
// 获取下一个可用端口
bool getReleasePort(short& port);



