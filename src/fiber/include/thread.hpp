#pragma once

#include <memory>
#include <string>
#include <functional>


namespace monsoon {
class Thread {
public:
    using ptr = std::shared_ptr<Thread>;

    Thread(std::function<void()> cb, const std::string &name);
    Thread(const Thread &) = delete;
    Thread& operator=(const Thread &) = delete;
    Thread(Thread &&) = delete;
    ~Thread();

    pid_t getId() const {return m_id;}
    const std::string& getName() const {return m_name;}
    void join();
    static Thread *GetThis();
    static const std::string &GetName();
    static void SetName(const std::string &name);

private:
    static void *run(void *args);
    
private:
    pid_t m_id;
    pthread_t m_thread;
    std::function<void()> m_cb;
    std::string m_name;
};
}