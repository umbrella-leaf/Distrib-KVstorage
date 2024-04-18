#pragma once

namespace monsoon {
// 禁用拷贝构造和拷贝赋值的基类，子类也无法拷贝，原因是子类拷贝需要先调用父类的拷贝函数
class NonCopyable {
public:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable& operator=(const NonCopyable &) = delete;
};
}