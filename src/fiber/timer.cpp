#include "timer.hpp"
#include "mutex.hpp"
#include "utils.hpp"
#include <cstdint>

namespace monsoon {
bool Timer::Comparator::operator() (const Timer::ptr &lhs, const Timer::ptr &rhs) const {
    if (!lhs && !rhs) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (!rhs) {
        return false;
    }
    if (lhs->m_next < rhs->m_next) {
        return true;
    }
    if (rhs->m_next < lhs->m_next) {
        return false;
    }
    return lhs.get() < rhs.get();
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recuring, TimerManager *manager)
    : m_recurring(recuring), m_ms(ms), m_cb(cb), m_manager(manager) {
  m_next = GetElapsedMs() + m_ms;
}

Timer::Timer(uint64_t next): m_next(next) {}

bool Timer::cancel() {
    RWMutex::WriteLock lock(m_manager->m_mutex);
    // 当前定时器有回调才可以取消
    if (m_cb) {
        m_cb = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

// 刷新定时器
bool Timer::refresh() {
    RWMutex::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    m_next = GetElapsedMs() + m_ms;
    m_manager->m_timers.emplace(shared_from_this());
    return true;
}

// 重置定时器，重新设定触发时间
bool Timer::reset(uint64_t ms, bool from_now) {
    if (m_ms == ms && !from_now) {
        return true;
    }
    RWMutex::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return true;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    uint64_t start = from_now ? GetElapsedMs() : (m_next - m_ms);
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;
}

TimerManager::TimerManager() {m_previousTime = GetElapsedMs();}

TimerManager::~TimerManager() {}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutex::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if (tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                           bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer() {
    RWMutex::ReadLock lock(m_mutex);
    m_tickle = false;
    if (m_timers.empty()) {
        return ~0ull;
    }
    const Timer::ptr &next = *m_timers.begin();
    uint64_t now_ms = GetElapsedMs();
    if (now_ms >= next->m_next) {
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
    uint64_t now_ms = GetElapsedMs();
    std::vector<Timer::ptr> expired;
    {
        RWMutex::ReadLock lock(m_mutex);
        if (m_timers.empty()) {
        return;
        }
    }
    RWMutex::WriteLock lock(m_mutex);
    if (m_timers.empty()) {
        return;
    }
    bool rollover = false;
    if (detectClockRollover(now_ms)) {
        rollover = true;
    }
    if (!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        return;
    }

    Timer::ptr now_timer(new Timer(now_ms));
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    while (it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }
    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);

    cbs.reserve(expired.size());
    for (auto &timer : expired) {
        cbs.push_back(timer->m_cb);
        if (timer->m_recurring) {
        // 循环计时，重新加入堆中
        timer->m_next = now_ms + timer->m_ms;
        m_timers.insert(timer);
        } else {
        timer->m_cb = nullptr;
        }
    }
}

void TimerManager::addTimer(Timer::ptr val, RWMutex::WriteLock &lock) {
    auto it = m_timers.insert(val).first;
    bool at_front = (it == m_timers.begin()) && !m_tickle;
    if (at_front) {
        m_tickle = true;
    }
    lock.unlock();
    if (at_front) {
        OnTimerInsertedAtFront();
    }
}

bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    if (now_ms < m_previousTime && now_ms < (m_previousTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previousTime = now_ms;
    return rollover;
}

bool TimerManager::hasTimer() {
    RWMutex::ReadLock lock(m_mutex);
    return !m_timers.empty();
}
}