#include "thread.hpp"
#include "utils.hpp"
#include <functional>
#include <stdexcept>

namespace monsoon {

static thread_local Thread* cur_thread = nullptr;
static thread_local std::string cur_thread_name = "UNKNOWN";

Thread::Thread(std::function<void()> cb, const std::string &name = "UNKNOWN"): m_cb(cb), m_name(name) {
    if (name.empty()) {
        m_name = "UNKNOWN";
    }

    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt) {
        std::cout << "pthread_create error,name:" << m_name << std::endl;
        throw std::logic_error("pthread_create");
    }
}

void *Thread::run(void *args) {
    Thread *thread = (Thread *)args;
    cur_thread = thread;
    cur_thread_name = thread->m_name;
    thread->m_id = monsoon::GetThreadId();
    // 给线程命名
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());
    std::function<void()> cb;
    cb.swap(thread->m_cb);
    // 启动回调函数
    cb();
    return 0;
}

Thread::~Thread() {
    if (m_thread) {
        pthread_detach(m_thread);
    }
}

void Thread::join() {
    if (m_thread) {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) {
            std::cout << "pthread_join error,name:" << m_name << std::endl;
            throw std::logic_error("pthread_join");
        }
        m_thread = 0;
    }
}

Thread *Thread::GetThis() {return cur_thread;}

const std::string &Thread::GetName() {return cur_thread_name;}

void Thread::SetName(const std::string &name) {
    if (name.empty()) {
        return;
    }
    if (cur_thread) {
        cur_thread->m_name = name;
    }
    cur_thread_name = name;
}
}