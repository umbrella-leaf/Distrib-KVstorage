#pragma once

#include <memory>
namespace monsoon {
template<typename T, typename X = void, int N = 0>
class Singleton {
public:
    // 这里并不是饿汉模式，而是懒汉模式，因为代码执行到这里时才初始化
    static T* GetInstance() {
        static T v;
        return &v;
    }
};

template<typename T, typename X = void, int N = 0>
class SingletonPtr {
public:
    static std::shared_ptr<T> GetInstance() {
        static std::shared_ptr<T> v(new T);
        return v;
    }
};
}