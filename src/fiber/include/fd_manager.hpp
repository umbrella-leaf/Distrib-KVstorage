#pragma once

#include "mutex.hpp"
#include "singleton.hpp"
#include <memory>
#include <vector>


namespace monsoon {
// 文件句柄上下文，管理文件句柄类型，阻塞，关闭，读写超时
class FdCtx: public std::enable_shared_from_this<FdCtx> {
public:
    using ptr = std::shared_ptr<FdCtx>;

    FdCtx(int fd);
    ~FdCtx();
    // 是否完成初始化
    bool isInit() const {return true;}
      // 是否是socket
    bool isSocket() const { return m_isSocket; }
    // 是否已经关闭
    bool isClose() const { return m_isClosed; }
    // 用户主动设置非阻塞
    void setUserNonblock(bool v) { m_userNonBlock = v; }
    // 用户是否主动设置了非阻塞
    bool getUserNonblock() const { return m_userNonBlock; }
    // 设置系统非阻塞
    void setSysNonblock(bool v) { m_sysNonblock = v; }
    // 获取系统是否非阻塞
    bool getSysNonblock() const { return m_sysNonblock; }
    // 设置超时时间
    void setTimeout(int type, uint64_t v);
    uint64_t getTimeout(int type);
private:
    bool init();

private:
    // 是否初始化
    bool m_isInit: 1;
    // 是否socket
    bool m_isSocket: 1;
    // 是否hook非阻塞
    bool m_sysNonblock: 1;
    // 是否用户主动设置非阻塞
    bool m_userNonBlock: 1;
    // 是否关闭
    bool m_isClosed: 1;
    // 文件句柄
    int m_fd;
    // 读超时时间(ms)
    uint64_t m_recvTimeout;
    // 写超时时间(ms)
    uint64_t m_sendTimeout;
};
// 文件句柄管理
class FdManager {
public:
    using RWMutexType = RWMutex;

    FdManager();
    // 获取/创建文件句柄，auto_create指定是否自动创建
    FdCtx::ptr get(int fd, bool auto_create = false);
    // 删除文件句柄
    void del(int fd);

private:
    // 读写锁
    RWMutexType m_mutex;
    // 文件句柄集合，通过数组下标来定位文件句柄
    std::vector<FdCtx::ptr> m_datas;
};

// 文件管理对象单例
using FdMgr = Singleton<FdManager>;
}