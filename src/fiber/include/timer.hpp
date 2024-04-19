#pragma once

#include "mutex.hpp"
#include <functional>
#include <memory>
#include <set>

namespace monsoon {
// 定时器管理类的前向声明
class TimerManager;
// 定时器类
class Timer: public std::enable_shared_from_this<Timer> {
    friend class TimerManager;
public:
    using ptr = std::shared_ptr<Timer>;

    bool cancel();
    bool refresh();
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recuring, TimerManager *manager);
    Timer(uint64_t next);

    // 是够是循环定时器
    bool m_recurring{false};
    // 执行周期
    uint64_t m_ms{0};
    // 精确执行时间
    uint64_t m_next{0};
    // 回调函数
    std::function<void()> m_cb;
    // 定时器管理器
    TimerManager *m_manager{nullptr};

private:
    struct Comparator {
        bool operator() (const Timer::ptr &lhs, const Timer::ptr &rhs) const;
    };
};

class TimerManager {
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
    Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                bool recurring = false);
    // 到最近一个定时器的时间间隔(ms)
    uint64_t getNextTimer();
    // 获取需要执行的定时器的回调函数列表
    void listExpiredCb(std::vector<std::function<void()>> &cbs);
    // 是否有定时器
    bool hasTimer();

protected:
    // 当有新的定时器加入到队列首部，执行该函数
    virtual void OnTimerInsertedAtFront() = 0;
    // 将定时器添加到集合
    void addTimer(Timer::ptr val, RWMutex::WriteLock &lock);

private:
    // 检测服务器时间是否被调后了
    bool detectClockRollover(uint64_t now_ms);

    RWMutex m_mutex;
    // 定时器集合，使用set以保证有序
    std::set<Timer::ptr, Timer::Comparator> m_timers;
    // 是否触发OnTimerInsertAtFront
    bool m_tickle{false};
    // 上次执行时间
    uint64_t m_previousTime{0};
};
}