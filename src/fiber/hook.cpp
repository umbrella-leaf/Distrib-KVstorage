#include "hook.hpp"
#include "fd_manager.hpp"
#include "fiber.hpp"
#include "iomanager.hpp"
#include <dlfcn.h>
#include <cstdarg>

namespace monsoon {
// 当前线程是否启用hook
static thread_local bool t_hook_enable{false};
static int g_tcp_connect_timeout = 5000;

#define HOOK_FUN(XX) \
  XX(sleep)          \
  XX(usleep)         \
  XX(nanosleep)      \
  XX(socket)         \
  XX(connect)        \
  XX(accept)         \
  XX(read)           \
  XX(readv)          \
  XX(recv)           \
  XX(recvfrom)       \
  XX(recvmsg)        \
  XX(write)          \
  XX(writev)         \
  XX(send)           \
  XX(sendto)         \
  XX(sendmsg)        \
  XX(close)          \
  XX(fcntl)          \
  XX(ioctl)          \
  XX(getsockopt)     \
  XX(setsockopt)

void hook_init() {
    static bool is_inited = false;
    if (is_inited) {
        return;
    }
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

// hook_init放在全局静态对象中，在main函数执行之前就会获取各个符号地址并执行hook
static uint64_t s_connect_timeout = -1;
struct _HookIniter {
    _HookIniter() {
        hook_init();
        s_connect_timeout = g_tcp_connect_timeout;
    }
};

static _HookIniter s_hook_initer;

bool is_hook_enable() {return t_hook_enable;}

void set_hook_enable(const bool flag) {t_hook_enable = flag;}

struct timer_info {
    int cancelled = 0;
};

template<typename OriginFunc, typename... Args>
static ssize_t do_io(int fd, OriginFunc fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args) {
    if (!t_hook_enable) {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 为当前文件描述符创建文件句柄
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    // 文件句柄创建失败则还是调用原函数
    if (!ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 如果文件已经关闭
    if (ctx->isClose()) {
        errno = EBADF;
        return -1;
    }
    // 不是socket，还是调用原函数
    if (!ctx->isSocket() || ctx->getUserNonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 获取对应的type(读/写)的超时时间
    uint64_t timeout = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    while (n == -1 && errno == EINTR) {
        // 读取操作被信号中断，继续尝试
        n = fun(fd, std::forward<Args>(args)...);
    }
    // 在未读到内容的情况下
    if (n == -1 && errno == EAGAIN) {
        // 数据未就绪
        IOManager *iom = IOManager::GetThis();
        Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);
        // 超时时间不为空，则注册定时器
        if (timeout != (uint64_t)-1) {
            timer = iom->addConditionTimer(
                timeout,
                [winfo, fd, iom, event]() {
                    auto t = winfo.lock();
                    if (!t || t->cancelled) {
                        return;
                    }
                    // 如果超时回调被触发，取消标志会被设置
                    t->cancelled = ETIMEDOUT;
                    iom->cancelEvent(fd, (Event)(event));
                },
                winfo);
        }
        // 调度后回到当前协程继续执行
        int rt = iom->addEvent(fd, (Event)(event));
        if (rt) {
            std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
            if (timer) {
                timer->cancel();
            }
            return -1;
        } else {
            Fiber::GetThis()->yield();
            if (timer) {
                timer->cancel();
            }
            // 在这里检查取消标志是否被设置
            // 如果被设置，说明超时回调被触发，应该直接结束调用
            // 否则说明此时没超时(也没读到内容)，就回到retry重新尝试i/o操作
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}

extern "C" {
#define XX(name) name##_fun name##_f = nullptr;
HOOK_FUN(XX);
#undef XX

unsigned int sleep(unsigned int seconds) {
    if (!t_hook_enable) {
        return sleep_f(seconds);
    }
    // 允许hook则直接让当前协程退出，并添加定时器
    // 由于addTimer不支持直接将当前协程加入任务队列，故需要手动指定scheduler操作
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    iom->addTimer(seconds * 1000, 
                std::bind((void(Scheduler::*)(Fiber::ptr, int))&IOManager::scheduler, iom, fiber, -1));  
    Fiber::GetThis()->yield();
    // 超时触发时，协程进入任务队列，被调度会回到这里
    return 0;
}

int usleep(useconds_t usec) {
    if (!t_hook_enable) {
        auto ret = usleep_f(usec);
        return 0;
    }
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    iom->addTimer(usec / 1000,
                    std::bind((void(Scheduler::*)(Fiber::ptr, int thread)) &IOManager::scheduler, iom, fiber, -1));
    Fiber::GetThis()->yield();
    return 0;
}

// nanosleep 在指定的纳秒数内暂停当前线程的执行
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!t_hook_enable) {
        return nanosleep_f(req, rem);
    }
    // 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    int timeout_s = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;
    iom->addTimer(timeout_s,
                    std::bind((void(Scheduler::*)(Fiber::ptr, int thread)) & IOManager::scheduler, iom, fiber, -1));
    Fiber::GetThis()->yield();
    return 0;
}

int socket(int domain, int type, int protocol) {
    if (!t_hook_enable) {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if (fd == -1) {
        return fd;
    }
    // 如果创建socket成功，就将其加入FdMANAGER中
    FdMgr::GetInstance()->get(fd, true);
    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms) {
    if (!t_hook_enable) {
        // 未hook则执行无超时connect(原版)
        return connect_f(fd, addr, addrlen);
    }
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if (!ctx || ctx->isClose()) {
        errno = EBADF;
        return -1;
    }
    if (!ctx->isSocket()) {
        return connect_f(fd, addr, addrlen);
    }
    // fd是否被显式设置为非阻塞模式
    if (ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }
    // 系统调用connect
    int n = connect_f(fd, addr, addrlen);
    if (n == 0) {
        // 连接成功则返回
        return 0;
    } else if (n != -1 || errno != EINPROGRESS) {
        return n;
    }
    // 返回EINPROGRESS表示正在进行，但是尚未完成
    IOManager *iom = IOManager::GetThis();
    Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if (timeout_ms != (uint64_t)-1) {
        // 添加条件定时器，超时则设置取消标志
        timer = iom->addConditionTimer(
            timeout_ms, 
            [winfo, fd, iom]() {
                auto t = winfo.lock();
                if (!t || t->cancelled) {
                    return;
                }
                // 定时时间到，设置取消标志并触发一次WRITE事件
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, WRITE);
            }, winfo);

    }

    // 添加WRITE事件，并yield，等待WRITE事件触发再继续执行
    int rt = iom->addEvent(fd, WRITE);
    if (rt == 0) {
        Fiber::GetThis()->yield();
        if (timer) {
            timer->cancel();
        }
        // 超时则结束调用
        if (tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        if (timer) {
            timer->cancel();
        }
        std::cout << "connect addEvent(" << fd << ", WRITE) error" << std::endl;
    }
    // 连接未超时
    int error = 0;
    socklen_t len = sizeof(int);
    // 获取套接字错误状态
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }
    if (!error) {
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return monsoon::connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
  int fd = do_io(s, accept_f, "accept", READ, SO_RCVTIMEO, addr, addrlen);
  if (fd >= 0) {
    FdMgr::GetInstance()->get(fd, true);
  }
  return fd;
}

// read/write: 最基本的I/O操作，无法指定标志位
// send/recv: 可以指定标志位
// sendto/recvfrom：用于UDP无连接通信，可以获取对端IP地址等信息
// readv/writev：分散读/聚集写，可以将读取内容放在多个缓冲区，不过需要自行管理缓冲区
// sendmsg：除了可以分散读/聚集写，指定标志以外，还可以指定额外的辅助信息

ssize_t read(int fd, void *buf, size_t count) { return do_io(fd, read_f, "read", READ, SO_RCVTIMEO, buf, count); }

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  return do_io(fd, readv_f, "readv", READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
  return do_io(sockfd, recv_f, "recv", READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
  return do_io(sockfd, recvfrom_f, "recvfrom", READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
  return do_io(sockfd, recvmsg_f, "recvmsg", READ, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
  return do_io(fd, write_f, "write", WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  return do_io(fd, writev_f, "writev", WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void *msg, size_t len, int flags) {
  return do_io(s, send_f, "send", WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
  return do_io(s, sendto_f, "sendto", WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
}

ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
  return do_io(s, sendmsg_f, "sendmsg", WRITE, SO_SNDTIMEO, msg, flags);
}

int close(int fd) {
    if (!t_hook_enable) {
        return close_f(fd);
    }
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if (ctx) {
        auto iom = IOManager::GetThis();
        if (iom) {
            iom->cancelAll(fd);
        }
        FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

int fcntl(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    switch (cmd) {
    case F_SETFL: {
        int arg = va_arg(args, int);
        va_end(args);
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
        if (!ctx || ctx->isClose() || !ctx->isSocket()) {
            return fcntl_f(fd, cmd, arg);
        }
        ctx->setUserNonblock(arg & O_NONBLOCK);
        if (ctx->getSysNonblock()) {
            arg |= O_NONBLOCK;
        } else {
            arg &= ~O_NONBLOCK;
        }
        return fcntl_f(fd, cmd, arg);
    }
    break;

    case F_GETFL: {
        va_end(args);
        int arg = fcntl(fd, cmd);
        FdCtx::ptr fd_ctx = FdMgr::GetInstance()->get(fd);
        if (!fd_ctx || fd_ctx->isClose() || !fd_ctx->isSocket()) {
            return arg;
        }
        // 这是让用户无感？
        if (fd_ctx->getUserNonblock()) {
            return arg | O_NONBLOCK;
        } else {
            return arg & ~O_NONBLOCK;
        }
    }
    break;

    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
#ifdef F_SETPIPE_SZ
    case F_SETPIPE_SZ:
#endif
    {
        int arg = va_arg(args, int);
        va_end(args);
        return fcntl_f(fd, cmd, arg);
    } 
    break;

    case F_GETFD:
    case F_GETOWN:
    case F_GETSIG:
    case F_GETLEASE:
#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
    {
        va_end(args);
        return fcntl_f(fd, cmd);
    } 
    break;

    case F_SETLK:
    case F_SETLKW:
    case F_GETLK: {
        struct flock *arg = va_arg(args, struct flock *);
        va_end(args);
        return fcntl_f(fd, cmd, arg);
    } 
    break;

    case F_GETOWN_EX:
    case F_SETOWN_EX: {
        struct f_owner_exlock *arg = va_arg(args, struct f_owner_exlock *);
        va_end(args);
        return fcntl_f(fd, cmd, arg);
    } 
    break;
    default:
        va_end(args);
        return fcntl_f(fd, cmd);
    }
}

int ioctl(int d, unsigned long int request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    if (FIONBIO == request) {
        bool user_nonblock = !!*(int *)arg;
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(d);
        if (!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
  return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
  if (!t_hook_enable) {
    return setsockopt_f(sockfd, level, optname, optval, optlen);
  }
  if (level == SOL_SOCKET) {
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
      FdCtx::ptr ctx = FdMgr::GetInstance()->get(sockfd);
      if (ctx) {
        const timeval *v = (const timeval *)optval;
        ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
      }
    }
  }
  return setsockopt_f(sockfd, level, optname, optval, optlen);
}

}
}