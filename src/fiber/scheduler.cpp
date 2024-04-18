#include "scheduler.hpp"
#include "fiber.hpp"
#include "hook.hpp"
#include "mutex.hpp"
#include "thread.hpp"
#include "utils.hpp"
#include <string>
#include <vector>

namespace monsoon {
// 当前线程调度器，同一调度器下所有线程共享同一个调度器实例
static thread_local Scheduler *cur_scheduler{nullptr};
// 当前线程的调度协程，每个线程一个
static thread_local Fiber *cur_scheduler_fiber{nullptr};

const std::string LOG_HEAD = "[scheduler]";

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name) {
    CondPanic(threads > 0, "threeads <= 0");

    m_useCaller = use_caller;
    m_name = name;
    // 如果caller线程参与调度，那么实际工作线程数为threads - 1
    if (use_caller) {
        std::cout << LOG_HEAD << "current thread as called thread" << std::endl;
        --threads;
        // 初始化caller线程的主协程
        Fiber::GetThis();
        std::cout << LOG_HEAD << "init caller thread's main fiber success" << std::endl;
        // 如果当前线程已有调度器，是不能够建立新的调度器的
        CondPanic(GetThis() == nullptr, "GetThis err:cur scheduler is not nullptr");
        // 设置当前线程未调度器线程(caller thread)
        cur_scheduler = this;
        // 初始化当前线程调度协程
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        std::cout << LOG_HEAD << "init caller thread's caller fiber success" << std::endl;

        Thread::SetName(m_name);
        cur_scheduler_fiber = m_rootFiber.get();
        m_rootThread = GetThreadId();
        m_threadIds.emplace_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
    std::cout << "-------scheduler init success-------" << std::endl;
}

Scheduler *Scheduler::GetThis() {return cur_scheduler;}
Fiber *Scheduler::GetMainFiber() {return cur_scheduler_fiber;}
void Scheduler::setThis() {cur_scheduler = this;}
Scheduler::~Scheduler() {
    CondPanic(m_isStopped, "Scheduler is not stopped");
    // 对于所有属于该调度器的线程，需要将cur_scheduler置空
    if (GetThis() == this) {
        cur_scheduler = nullptr;
    }
}

// 启动调度器
void Scheduler::start() {
    std::cout << LOG_HEAD << "scheduler start" << std::endl;
    Mutex::Lock lock(m_mutex);
    if (m_isStopped) {
        std::cout << "scheduler has stopped" << std::endl;
        return;
    }
    CondPanic(m_threadPool.empty(), "thread pool is not empty");
    m_threadPool.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; ++i) {
        m_threadPool[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_threadIds.emplace_back(m_threadPool[i]->getId());
    }
}

// 协程调度函数
void Scheduler::run() {
    std::cout << LOG_HEAD << "begin run" << std::endl;
    set_hook_enable(true);
    setThis();
    // 当前线程不是caller线程，也就是是工作线程
    // 那就需要初始化当前线程的主协程，同时也作为调度协程(工作线程的主协程和调度协程是同一个协程)
    if (GetThreadId() != m_rootThread) {
        cur_scheduler_fiber = Fiber::GetThis().get();
    }
    // 创建idle协程，在任务队列为空时进入
    Fiber::ptr idleFiber(new Fiber(std::bind(&Scheduler::idle, this)));
    // 用于储存待执行的回调任务协程
    Fiber::ptr cbFiber;
    // 复用对象
    SchedulerTask task;
    while (true) {
        task.reset();
        // 是否通知其他线程进行任务调度
        bool tickle_me = false;
        {
            Mutex::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 任务队列不为空
            while (it != m_tasks.end()) {
                // 发现队头的任务协程不是交由当前线程进行调度，就要通知其他线程进行调度
                if (it->m_thread != -1 && it->m_thread != GetThreadId()) {
                    ++it;
                    tickle_me = true;
                    continue;
                }
                CondPanic(it->m_fiber || it->m_cb, "task is nullptr");
                if (it->m_fiber) {
                    // 待运行协程状态必须是就绪
                    CondPanic(it->m_fiber->getState() == Fiber::READY, "fiber task state error");
                }
                // 找到可执行任务，将其从任务队列取出，调度器活跃线程数+1
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }   
            // 在有任务不由当前线程调度/找到可调度任务但队列不为空的情况下，都要唤醒其他线程
            tickle_me |= (it != m_tasks.end());
        }
        if (tickle_me) {
            tickle();
        }

        if (task.m_fiber) {
            task.m_fiber->resume();
            // 执行结束，调度器活跃线程数-1
            --m_activeThreadCount;
            // 清除任务
            task.reset();
        } else if (task.m_cb) {
            // 如果回调任务协程不为空，就更换协程中的回调
            if (cbFiber) {
                cbFiber->reset(task.m_cb);
            // 否则新建任务协程
            } else {
                cbFiber.reset(new Fiber(task.m_cb));
            }
            task.reset();
            cbFiber->resume();
            --m_activeThreadCount;
            // 执行完毕，清空任务
            cbFiber.reset();
        } else {
            // 没能取出任务，任务队列为空
            // 如果idle协程已经终止，说明整个调度器也终止了，应该结束调度
            if (idleFiber->getState() == Fiber::TERM) {
                std::cout << "idle fiber term" << std::endl;
                break;
            }
            // 否则不断空轮转，调度协程->idle协程->调度协程
            ++m_idleThreadCount;
            idleFiber->resume();
            --m_idleThreadCount;
        }
    }
    std::cout << "run exit" << std::endl;
}

void Scheduler::tickle() { std::cout << "tickle" << std::endl; }

bool Scheduler::stopping() {
    Mutex::Lock lock(m_mutex);
    // 必须满足调度器被停止，全部任务结束，活跃线程数为0，才可以真正结束全部调度过程
    return m_isStopped && m_tasks.empty() && m_activeThreadCount == 0;
}

void Scheduler::idle() {
    while (!stopping()) {
        Fiber::GetThis()->yield();
    }
}

// caller线程的调度协程在这里才启动
void Scheduler::stop() {
    std::cout << LOG_HEAD << "stop" << std::endl;
    if (stopping()) {
        return;
    }
    m_isStopped = true;
    // stop操作在use_caller情况下只能由caller发起
    if (m_useCaller) {
        CondPanic(GetThis() == this, "cue thread is not caller thread");
    } else {
        CondPanic(GetThis() != this, "cur thread is caller thread");
    }

    for (size_t i = 0; i < m_threadCount; ++i) {
        tickle();
    }
    if (m_rootFiber) {
        tickle();
    }
    
    if (m_rootFiber) {
        // 启动caller线程的调度协程
        m_rootFiber->resume();
        std::cout << "root fiber end" << std::endl;
    }

    std::vector<Thread::ptr> threads;
    {
        Mutex::Lock lock(m_mutex);
        threads.swap(m_threadPool);
    }
    for (auto &thread: threads) {
        thread->join();
    }
}
}
