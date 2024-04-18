#include "fiber.hpp"
#include "scheduler.hpp"
#include "utils.hpp"
#include <ucontext.h>
#include <atomic>

namespace monsoon {
const bool DEBUG = true;
// 当前线程正在运行的协程
static thread_local Fiber *cur_fiber{nullptr};
// 当前线程的主协程
static thread_local Fiber::ptr cur_thread_fiber{nullptr};
// 用于生成协程Id
static std::atomic<uint64_t> cur_fiber_id{0};
// 统计当前协程数
static std::atomic<uint64_t> fiber_count{0};
// 协议栈默认大小 128k
static int g_fiber_stack_size{128 * 1024};
// 内存分配器
class StackAllocator {
public:
    static void *Alloc(size_t size) {return malloc(size);}
    static void Delete(void *vp, size_t size) {return free(vp);}
};

Fiber::Fiber() {
    SetThis(this);
    m_state = RUNNING;
    CondPanic(getcontext(&m_ctx) == 0, "getcontext error");
    ++fiber_count;
    m_id = cur_fiber_id++;
    std::cout << "[fiber] create fiber , id = " << m_id << std::endl;
}

void Fiber::SetThis(Fiber *f) {cur_fiber = f;}
Fiber::ptr Fiber::GetThis() {
    if (cur_fiber) {
        return cur_fiber->shared_from_this();
    }
    // 创建主协程并初始化
    Fiber::ptr main_fiber(new Fiber);
    CondPanic(cur_fiber == main_fiber.get(), "cur fiber need to be main_fiber");
    cur_thread_fiber = main_fiber;
    return cur_fiber->shared_from_this();
}

Fiber::Fiber(std::function<void()> cb, size_t stackSize, bool run_in_shceduler)
    : m_id(cur_fiber_id++), m_cb(cb), m_isRunInScheduler(run_in_shceduler) {
    ++fiber_count;
    m_stackSize = stackSize > 0 ? stackSize : g_fiber_stack_size;
    m_stack = StackAllocator::Alloc(m_stackSize);
    CondPanic(getcontext(&m_ctx) == 0, "getcontect error");
    // 初始化协程上下文
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stackSize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

void Fiber::resume() {
    CondPanic(m_state != TERM && m_state != RUNNING, "Only ready fiber can be resumed");
    SetThis(this);
    m_state = RUNNING;

    if (m_isRunInScheduler) {
        // 当前协程参与调度，必是任务协程，那么resume时与调度协程进行swap
        CondPanic(0 == swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx), 
                 "run_in_scheduler = true, swapcontext error");
    } else {
        CondPanic(0 == swapcontext(&(cur_thread_fiber->m_ctx), &m_ctx), "run_in_scheduler = false, swapcontext error");
    }
}

void Fiber::yield() {
    CondPanic(m_state == TERM || m_state == RUNNING, "Only ready fiber can be resumed");
    SetThis(cur_thread_fiber.get());
    if (m_state != TERM) {
        m_state = READY;
    }

    if (m_isRunInScheduler) {
        // 当前协程参与调度，必是任务协程，那么yield时与调度协程进行swap
        CondPanic(0 == swapcontext(&m_ctx, &(Scheduler::GetMainFiber()->m_ctx)), 
                 "run_in_scheduler = true, swapcontext error");
    } else {
        CondPanic(0 == swapcontext(&m_ctx,&(cur_thread_fiber->m_ctx)), "run_in_scheduler = false, swapcontext error");
    }
}

void Fiber::MainFunc() {
    Fiber::ptr cur = GetThis();
    CondPanic(cur != nullptr, "cur is nullptr");

    cur->m_cb();
    // 真实回调结束和，函数置空，状态变为终止
    cur->m_cb = nullptr;
    cur->m_state = TERM;
    // 最后要yield到主协程，回不来了，因此这里的cur必须手动释放一次引用计数
    // 不然可能造成内存泄漏
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();
}

// 协程重置，复用已结束协程的栈空间来创建新协程
void Fiber::reset(std::function<void()> cb) {
    CondPanic(m_stack, "stack is nullptr");
    // 只有终止的协程才能复用
    CondPanic(m_state == TERM, "state isn't TERM");
    m_cb = cb;
    CondPanic(0 == getcontext(&m_ctx), "getcontext failed");
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stackSize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY;
}

Fiber::~Fiber() {
    --fiber_count;
    if (m_stack) {
        // 有栈空间，说明是非主协程
        CondPanic(m_state == TERM, "fiber state should be term");
        StackAllocator::Delete(m_stack, m_stackSize);
    } else {
        // 没有栈空间，那一定是主协程，同时也一定没有指定回调
        // 并且析构前一直处于RUNNING状态
        CondPanic(!m_cb, "main fiber no callback");
        CondPanic(m_state == RUNNING, "main fiber state should always be running");

        Fiber *cur = cur_fiber;
        if (cur == this) {
            SetThis(nullptr);
        }
    }
}

}