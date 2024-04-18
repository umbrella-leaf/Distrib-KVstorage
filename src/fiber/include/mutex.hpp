#pragma once

#include "noncopyable.hpp"
#include "utils.hpp"
#include <pthread.h>
#include <semaphore.h>

namespace monsoon {
// 信号量
class Semaphore: NonCopyable {
public:
    Semaphore(uint32_t count = 0);
    ~Semaphore();

    void wait();
    void notify();

private:
    sem_t m_semaphore;
};

// 局部锁
template<typename T>
struct ScopedLockImpl {
public:
    ScopedLockImpl(T &mutex): m_mutex(mutex) {
        m_mutex.lock();
        m_isLocked = true;
    }
    // 在未上锁时才上锁
    void lock() {
        if (!m_isLocked) {
            m_mutex.lock();
            m_isLocked = true;
        }
    }
    // 在已上锁时才解锁
    void unlock() {
        if (m_isLocked) {
            m_mutex.unlock();
            m_isLocked = false;
        }
    }
    ~ScopedLockImpl() {
        unlock();
    }

private:
    T &m_mutex;
    // 是否已经上锁
    bool m_isLocked;
};

template<typename T>
struct ReadScopedLockImpl {
public:
    ReadScopedLockImpl(T &mutex): m_mutex(mutex) {
        m_mutex.rdlock();
        m_isLocked = true;
    }
    ~ReadScopedLockImpl() {unlock();}
    void lock() {
        if (!m_isLocked) {
            m_mutex.rdlock();
            m_isLocked = true;
        }
    }
    void unlock() {
        if (m_isLocked) {
            m_mutex.unlock();
            m_isLocked = false;
        }
    }

private:
    T &m_mutex;
    bool m_isLocked;
};

template<typename T>
struct WriteScopedLockImpl {
public:
    WriteScopedLockImpl(T &mutex): m_mutex(mutex) {
        m_mutex.wrlock();
        m_isLocked = true;
    }
    ~WriteScopedLockImpl() {unlock();}
    void lock() {
        if (!m_isLocked) {
            m_mutex.wrlock();
            m_isLocked = true;
        }
    }
    void unlock() {
        if (m_isLocked) {
            m_mutex.unlock();
            m_isLocked = false;
        }
    }

private:
    T &m_mutex;
    bool m_isLocked;
};

class Mutex: NonCopyable {
public:
    using Lock = ScopedLockImpl<Mutex>;

    Mutex() {
        CondPanic(0 == pthread_mutex_init(&m_mutex, nullptr), "lock init success");
    }

    void lock() {
        CondPanic(0 == pthread_mutex_lock(&m_mutex), "lock error");
    }

    void unlock() {
        CondPanic(0 == pthread_mutex_unlock(&m_mutex), "unlock error");
    }

    ~Mutex() {
        CondPanic(0 == pthread_mutex_destroy(&m_mutex), "destroy lock error");
    }

private:
    pthread_mutex_t m_mutex;
};

class RWMutex: NonCopyable {
public:
    // 局部读锁
    using ReadLock = ReadScopedLockImpl<RWMutex>;
    // 局部写锁
    using WriteLock = WriteScopedLockImpl<RWMutex>;

    RWMutex() {pthread_rwlock_init(&m_rwmutex, nullptr);}
    ~RWMutex() {pthread_rwlock_destroy(&m_rwmutex);}

    void rdlock() {pthread_rwlock_rdlock(&m_rwmutex);}
    void wrlock() {pthread_rwlock_wrlock(&m_rwmutex);}
    void unlock() {pthread_rwlock_unlock(&m_rwmutex);}

private:
    pthread_rwlock_t m_rwmutex;
};
}