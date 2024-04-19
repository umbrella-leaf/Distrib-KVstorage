#pragma once

#include "scheduler.hpp"
#include "timer.hpp"
#include <string>

namespace monsoon {
enum Event {
    NONE = 0x0,
    READ = 0x1,
    WRITE = 0x4,
};

struct EventContext {
    Scheduler *scheduler{nullptr};
    Fiber::ptr fiber;
    std::function<void()> cb;
};

class FdContext {
    friend class IOManager;
public:
    // 获取事件上下文
    EventContext &getEventContext(Event event);
    // 重置事件上下文
    void resetEventContext(EventContext &ctx);
    // 触发事件
    void triggerEvent(Event event);

private:
    EventContext read;
    EventContext write;
    int fd{0};
    Event events{NONE};
    Mutex mutex;
};

class IOManager: public Scheduler, public TimerManager {
public:
    using ptr = std::shared_ptr<IOManager>;

    IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "IOManager");
    ~IOManager();
    // 添加事件
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // 删除事件
    bool delEvent(int fd, Event event);
    // 取消事件
    bool cancelEvent(int fd, Event event);
    // 取消所有事件
    bool cancelAll(int fd);
    static IOManager *GetThis();

protected:
    // 通知调度器有任务要调度
    void tickle() override;
    // 判断是否可以停止
    bool stopping() override;
    // idle协程
    void idle() override;
    // 判断是否可以停止
    bool stopping(uint64_t &timeout);
    void OnTimerInsertedAtFront() override;
    void contextResize(size_t size);

private:
    int m_epfd{0};
    int m_tickleFds[2];
    // 正在等待执行的IO事件数量
    std::atomic<size_t> m_pendingEventCount{0};
    RWMutex m_mutex;
    // 文件描述符上下文集合
    std::vector<FdContext *> m_fdContexts;
};
}