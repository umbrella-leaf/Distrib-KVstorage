#include <iostream>
#include <string>
#include <vector>
#include "monsoon.h"
#include "thread.hpp"

void func1() {
    std::cout << "name: " << monsoon::Thread::GetThis()->GetName() << ",id: " << monsoon::GetThreadId() << std::endl;
}

void func2() {
    std::cout << "name: " << monsoon::Thread::GetName() << ",id: " << monsoon::GetThreadId() << std::endl;
}

int main(int argc, char **argv) {
    std::vector<monsoon::Thread::ptr> tpool;
    for (int i = 0; i < 5; ++i) {
        monsoon::Thread::ptr t(new monsoon::Thread(&func1, "name_" + std::to_string(i)));
        tpool.emplace_back(t);
    }

    for (int i = 0; i < 5; ++i) {
        tpool[i]->join();
    }
    std::cout << "-----thread_test end-----" << std::endl;
}