#include "fd_manager.hpp"
#include "hook.hpp"
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>


namespace monsoon {

FdCtx::FdCtx(int fd)
    : m_isInit(false),
      m_isSocket(false),
      m_sysNonblock(false),
      m_userNonBlock(false),
      m_isClosed(false),
      m_fd(fd),
      m_recvTimeout(-1),
      m_sendTimeout(-1) {
    init();
}

FdCtx::~FdCtx() {}

bool FdCtx::init() {
    if (m_isInit) {
        return true;
    }
    m_recvTimeout = -1;
    m_sendTimeout = -1;

    // 获取文件状态信息
    struct stat fd_stat;
    if (-1 == fstat(m_fd, &fd_stat)) {
        m_isInit = false;
        m_isSocket = false;
    } else {
        m_isInit = true;
        m_isSocket = S_ISSOCK(fd_stat.st_mode);
    }

    // 对socket设置非阻塞
    if (m_isSocket) {
        int flags = fcntl_f(m_fd, F_GETFL, 0);
        if (!(flags & O_NONBLOCK)) {
            fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);
        }
        m_sysNonblock = true;
    } else {
        m_sysNonblock = false;
    }

    m_userNonBlock = false;
    m_isClosed = false;
    return m_isInit;
}

void FdCtx::setTimeout(int type, uint64_t v) {
    if (type == SO_RCVTIMEO) {
        m_recvTimeout = v;
    } else {
        m_sendTimeout = v;
    }
}

uint64_t FdCtx::getTimeout(int type) {
    if (type == SO_RCVTIMEO) {
        return m_recvTimeout;
    } else {
        return m_sendTimeout;
    }
}

FdManager::FdManager() {m_datas.resize(64);}

FdCtx::ptr FdManager::get(int fd, bool auto_create) {
    if (fd == -1) {
        return nullptr;
    }
    RWMutexType::ReadLock lock(m_mutex);
    // 如果传入的文件描述符比描述符集合大小还大(按下标寻址)
    if ((int)m_datas.size() <= fd) {
        // 不自动创建则返回空指针
        if (auto_create == false) {
            return nullptr;
        }
    // 否则
    } else {
        // 该位置文件句柄不为空，或者(为空但)不自动创建
        if (m_datas[fd] || !auto_create) {
            // 直接返回对应位置的文件句柄
            return m_datas[fd];
        }
    }
    lock.unlock();
    // 到这里的有两种情况，一是fd太大但指定了自动创建，需要扩容；
    // 二是fd对应的文件句柄为空，但指定自动创建，不需要扩容
    RWMutexType::WriteLock lock2(m_mutex);
    // 新建文件句柄
    FdCtx::ptr ctx(new FdCtx(fd));
    if (fd >= (int)m_datas.size()) {
        m_datas.resize(fd * 1.5);
    }
    m_datas[fd] = ctx;
    return ctx;
}

void FdManager::del(int fd) {
    RWMutexType::WriteLock lock(m_mutex);
    if ((int)m_datas.size() <= fd) {
        return;
    }
    m_datas[fd].reset();
}
}