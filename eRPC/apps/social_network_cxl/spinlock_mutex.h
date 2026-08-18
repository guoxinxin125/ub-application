/**
 * @file spinlock_mutex.h
 * @brief Simple spinlock mutex implementation
 */
#pragma once

#include <mutex>

class spinlock_mutex {
private:
    std::mutex mtx;

public:
    void lock() { mtx.lock(); }
    void unlock() { mtx.unlock(); }
    bool try_lock() { return mtx.try_lock(); }
};
