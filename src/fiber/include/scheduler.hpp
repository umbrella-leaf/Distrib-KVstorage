#pragma once

#include "fiber.hpp"
#include "mutex.hpp"
#include "thread.hpp"
#include <functional>
#include <memory>
#include <vector>
#include <list>
#include <atomic>

namespace monsoon {
class SchedulerTask {
public:
    friend class Scheduler;
    SchedulerTask(): m_thread(-1) {}
    SchedulerTask(Fiber::ptr f, int t): m_fiber(f), m_thread(t) {}
    SchedulerTask(Fiber::ptr *f, int t) {
        m_fiber.swap(*f);
        m_thread = t;
    }
    SchedulerTask(std::function<void()> f, int t) {
        m_cb = f;
        m_thread = t;
    }
    // 清空任务
    void reset() {
        m_fiber = nullptr;
        m_cb = nullptr;
        m_thread = -1;
    }

private:
    Fiber::ptr m_fiber;
    std::function<void()> m_cb;
    int m_thread;
};

// N->M协程调度器。N个线程共享一个协程任务队列
class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;

    Scheduler(size_t threads = -1, bool use_caller = true, const std::string &name = "Scheduler");
    // 需要继承，因此析构函数设为虚函数
    virtual ~Scheduler();
    const std::string &getName() const {return m_name;}
    // 获取当前线程调度器
    static Scheduler* GetThis();
    // 获取当前线程的调度器协程
    static Fiber* GetMainFiber();

    // 添加调度任务
    template<typename TaskType>
    void scheduler(TaskType task, int thread = -1) {
        bool isNeedTickle = false;
        {
            Mutex::Lock lock(m_mutex);
            isNeedTickle = scheduleNoLock(task, thread);
        }
        if (isNeedTickle) {
            tickle();
        }
    }
    // 启动调度器
    void start();
    // 停止调度器，等待所有任务结束
    void stop();

protected:
    // 通知调度器任务到达
    virtual void tickle();
    // 协程调度函数。默认启用hook
    void run();
    // 无任务时执行idle协程
    virtual void idle();
    // 返回是否可以停止
    virtual bool stopping();
    // 设置当前线程调度器
    void setThis();
    // 返回是否有空闲进程
    bool isHasIdleThreads() {return m_idleThreadCount > 0;}

private:
    // 无锁下，添加调度任务
    template<typename TaskType>
    bool scheduleNoLock(TaskType t, int thread) {
        bool isNeedTickle = m_tasks.empty();
        SchedulerTask task(t, thread);
        if (task.m_fiber || task.m_cb) {
            m_tasks.emplace_back(task);
        }
        return isNeedTickle;
    }
    // 调度器名称
    std::string m_name;
    // 互斥锁
    Mutex m_mutex;
    // 线程池
    std::vector<Thread::ptr> m_threadPool;
    // 任务队列
    std::list<SchedulerTask> m_tasks;
    // 线程池id数组
    std::vector<int> m_threadIds;
    // 工作线程数量，不包含caller线程
    size_t m_threadCount{0};
    // 活跃线程数量(正在执行任务的线程数量)
    std::atomic<size_t> m_activeThreadCount{0};
    // IDLE线程数量
    std::atomic<size_t> m_idleThreadCount{0};
    // 是否是use_caller;
    bool m_useCaller;
    // 如果caller线程参与调度，caller线程的调度协程
    Fiber::ptr m_rootFiber;
    // 如果caller线程参与调度，caller线程id
    int m_rootThread{0};
    // 调度停止标志
    bool m_isStopped{false};
};
}