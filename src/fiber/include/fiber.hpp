#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <sys/ucontext.h>
#include <utils.hpp>

namespace monsoon {
class Fiber: public std::enable_shared_from_this<Fiber> {
public:
    using ptr = std::shared_ptr<Fiber>;
    // Fiber状态机
    enum State {
        // 就绪态，刚创建或者yield之后的状态
        READY,
        // 运行态，resumr之后的状态
        RUNNING,
        // 结束态，协程的回调函数执行完毕之后的状态
        TERM
    };

private:
    // 无参构造，负责构造主协程，每个线程有且只有一个主协程
    Fiber();

public:
    // 构造新的非主子协程
    Fiber(std::function<void()> cb, size_t stackSize = 0, bool run_in_shceduler = true);
    ~Fiber();
    // 重置协程状态，复用栈空间
    void reset(std::function<void()> cb);
    // 切换协程到运行态，并保存主协程上下文
    void resume();
    // 让出协程执行权
    void yield();
    // 获取协程Id
    uint64_t getId() const {return m_id;}
    // 获取协程状态
    State getState() const {return m_state;}

    // 设置当前正在运行的协程
    static void SetThis(Fiber* f);
    // 获取当前正在运行的协程
    static Fiber::ptr GetThis();
    // 协程总数
    static uint64_t TotalFiberNum();
    // 协程回调函数，执行结束后yield回主协程
    static void MainFunc();
    // 获取当前协程ID
    static uint64_t GetCurFiberID();

private:
    // 协程ID
    uint64_t m_id{0};
    // 协程栈大小
    uint32_t m_stackSize{0};
    // 协程状态
    State m_state{READY};
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈地址
    void *m_stack{nullptr};
    // 协程回调函数
    std::function<void()> m_cb;
    // 本协程是否参与调度
    bool m_isRunInScheduler;
};
}