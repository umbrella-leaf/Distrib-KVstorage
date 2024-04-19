#pragma once

#include "iomanager.hpp"
#include <sys/epoll.h>
#include <cstring>
#include <fcntl.h>

namespace monsoon {
EventContext &FdContext::getEventContext(Event event) {
    switch (event) {
        case READ:
            return read;
        case WRITE:
            return write;
        default:
            CondPanic(false, "getContext error: unknown event");
    }
    throw std::invalid_argument("getContext invalid event");
}

void FdContext::resetEventContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void FdContext::triggerEvent(Event event) {
    CondPanic(events & event, "event hasn't been registered");
    events = (Event)(event & ~event);
    EventContext &ctx = getEventContext(event);
    if (ctx.cb) {
        ctx.scheduler->scheduler(ctx.cb);
    } else {
        ctx.scheduler->scheduler(ctx.fiber);
    }
    resetEventContext(ctx);
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name): Scheduler(threads, use_caller, name) {
    m_epfd = epoll_create(5000);
    int ret = pipe(m_tickleFds);
    CondPanic(ret == 0, "pipe error");

    // 注册pipe读句柄事件，用于tickle调度线程
    epoll_event event{};
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];
    // 设置非阻塞
    ret = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    CondPanic(ret == 0, "set fd nonblock error");
    // 注册管道读描述符
    ret = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    CondPanic(ret == 0, "epoll_ctl error");

    contextResize(32);
    // 启动scheduler，开始协程调度
    start();
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    FdContext *fd_ctx = nullptr;
    RWMutex::ReadLock lock(m_mutex);
    // 找到对应fd的FdContext，没有则创建
    if ((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        lock.unlock();
    } else {
        lock.unlock();
        RWMutex::WriteLock lock2(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    // 同一个fd不允许重复注册相同事件
    Mutex::Lock ctxLock(fd_ctx->mutex);
    CondPanic(!(fd_ctx->events & event), "addevent error, fd = " + std::to_string(fd));
    // 有注册事件则修改，没有则新增
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent{};
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    int ret = epoll_ctl(m_epfd, op, fd, &epevent);
    if (ret) {
        std::cout << "addevent: epoll ctl error" << std::endl;
        return -1;
    }
    // 待执行IO数量+1
    ++m_pendingEventCount;

    // 赋值fd对应的文件上下文相应的EventContext
    fd_ctx->events = (Event)(fd_ctx->events | event);
    EventContext &event_ctx = fd_ctx->getEventContext(event);
    CondPanic(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb, "event_ctx must be nullptr");

    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) {
        // 如设置回调函数
        event_ctx.cb.swap(cb);
    } else {
        // 没有设置回调函数则将当前协程设置为回调任务
        event_ctx.fiber = Fiber::GetThis();
        CondPanic(event_ctx.fiber->getState() == Fiber::RUNNING, "current fiber state must be RUNNING!");
    }
    std::cout << "add event success, fd = " << fd << std::endl;
    return 0;
}

// 删除事件，不触发回调
bool IOManager::delEvent(int fd, Event event) {
    RWMutex::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    Mutex::Lock ctxLock(fd_ctx->mutex);
    // 对应fd上下文必须注册了该事件
    if (!(fd_ctx->events & event)) {
        return false;
    }
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent{};
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;
    
    int ret = epoll_ctl(m_epfd, op, fd, &epevent);
    if (ret) {
        std::cout << "delevent: epoll_ctl error" << std::endl;
        return false;
    }
    --m_pendingEventCount;
    fd_ctx->events = new_events;
    EventContext &event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

// 取消事件，同时触发回调
bool IOManager::cancelEvent(int fd, Event event) {
    RWMutex::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    Mutex::Lock ctxLock(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        return false;
    }
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent{};
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;
    
    int ret = epoll_ctl(m_epfd, op, fd, &epevent);
    if (ret) {
        std::cout << "cancelevent: epoll_ctl error" << std::endl;
        return false;
    }
    // 删除前触发
    fd_ctx->triggerEvent(event);
    --m_pendingEventCount;
    return true; 
}

bool IOManager::cancelAll(int fd) {
    RWMutex::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        // 找不到当前事件，返回
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    Mutex::Lock ctxLock(fd_ctx->mutex);
    if (!fd_ctx->events) {
        return false;
    }

    int op = EPOLL_CTL_DEL;
    epoll_event epevent{};
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;
    
    int ret = epoll_ctl(m_epfd, op, fd, &epevent);
    if (ret) {
        std::cout << "cancelalleventx: epoll_ctl error" << std::endl;
        return false;
    }
    // 触发全部已注册事件
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }
    CondPanic(fd_ctx->events == 0, "fd not totally clear");
    return true;
}

IOManager* IOManager::GetThis() {return dynamic_cast<IOManager *>(Scheduler::GetThis());}

// 通知调度器有任务到来
void IOManager::tickle() {
    if (!isHasIdleThreads()) {
        // 没有空闲调度进程(也就是在idle状态的线程)，就不做任何事
        return;
    }
    // 写pipe管道，使得idle协程凑够epoll_wait退出，开始调度任务
    int rt = write(m_tickleFds[1], "T", 1);
    CondPanic(rt == 1, "write pipe error");
}

// 调度线程如果没拿到任务，就进入idle协程，唤醒的时机有:
// 1. 被其他线程tickle; 2. 有注册事件被触发; 3. 有定时器超时
void IOManager::idle() {
    // 最多检测256个事件
    const uint64_t MAX_EVENTS = 256;
    epoll_event *events = new epoll_event[MAX_EVENTS]();
    // 共享指针以防忘记删除
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {delete[] ptr;});

    while (true) {
        // 获取下一个定时器超时时间，同时判断调度器是否已经停止，以便及时结束idle协程
        uint64_t next_timeout = 0;
        if (stopping(next_timeout)) {
            std::cout << "name=" << getName() << "idle stopping exit";
            break;
        }
        // 阻塞等待，等待事件发生，或者被tickle唤醒/定时器超时
        int ret = 0;
        do {
            static const int MAX_TIMEOUT = 5000;
            // 定时器集合不为空，则计算需要等待的超时时间
            if (next_timeout != ~0ull) {
                next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
            } else {
                next_timeout = MAX_TIMEOUT;
            }
            // 阻塞等待事件就绪，用epoll_wait来代理超时
            ret = epoll_wait(m_epfd, events, MAX_EVENTS, (int)next_timeout);
            if (ret < 0) {
                if (errno == EINTR) {
                    // 系统调用被信号中断
                    continue;
                }
                std::cout << "epoll_wait [" << m_epfd << "] errno,err: " << errno << std::endl;
                break;
            } else {
                break;
            }
        } while (true);

        // 收集全部超时定时器，执行回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if (!cbs.empty()) {
            for (const auto& cb: cbs) {
                scheduler(cb);
            }
            cbs.clear();
        }

        for (int i = 0; i < ret; ++i) {
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0]) {
                // pipe管道内数据只用来唤醒idle线程，读完即可
                uint8_t dummy[256];
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }
            // 通过epoll_event的私有指针获取FdContext
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            Mutex::Lock ctxLock(fd_ctx->mutex);
            // 错误事件/挂起事件，都视为读+写，否则注册的事件可能永远不会触发
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                std::cout << "error events" << std::endl;
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            // 实际发生的事件类型
            int real_events = NONE;
            if (event.events & EPOLLIN) {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) {
                real_events |= WRITE;
            }
            if ((fd_ctx->events & real_events) == NONE) {
                // 实际事件与触发事件无交集，不用做任何事
                continue;
            }
            // 否则剔除触发的实际事件，将剩余事件重新加入epoll_wait
            int left_events = (fd_ctx->events & ~real_events);
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events;

            int ret2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (ret2) {
                std::cout << "epoll_wait [" << m_epfd << "] errno,err: " << errno << std::endl;
                continue;
            }
            // 处理已就绪事件(实际是加入调度器任务队列，等待执行)
            if (real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }
        // 处理结束，idle协程应该yield，转回调度协程，然后去调度任务
        // 同样，为避免回不来，必须手动清除一次共享指针的引用计数
        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();
        raw_ptr->yield();
    }
}

bool IOManager::stopping() {
    uint64_t timeout = 0;
    return stopping(timeout);
}

bool IOManager::stopping(uint64_t &timeout) {
    // ~0ull代表定时器集合为空，如果当前时间已经大于首个定时器超时时间，返回0
    timeout = getNextTimer();
    // 停止条件为：定时器集合为空+没有任何读写事件+Scheduler停止(执行了stop方法+任务队列空+没有活跃线程)
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size);
    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        // 每个文件上下文先初始化
        if (!m_fdContexts[i]) {
            m_fdContexts[i] = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

void IOManager::OnTimerInsertedAtFront() {tickle();}
}
