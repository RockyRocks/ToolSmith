#include <core/ThreadPool.h>

ThreadPool::ThreadPool(size_t n) {
    if (n == 0) n = 1;
    m_Stopping = false;
    for (size_t i = 0; i < n; i++) {
        m_Workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_Mtx);
                    m_Cv.wait(lock, [this] { return m_Stopping || !m_Tasks.empty(); });
                    if (m_Stopping && m_Tasks.empty()) return;
                    task = std::move(m_Tasks.front());
                    m_Tasks.pop();
                }
                task();
            }
        });
    }
}

ThreadPool& ThreadPool::Shared(size_t n) {
    static ThreadPool pool(n == 0 ? std::thread::hardware_concurrency() : n);
    return pool;
}

ThreadPool::~ThreadPool() { Shutdown(); }

void ThreadPool::Shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_Mtx);
        m_Stopping = true;
    }
    m_Cv.notify_all();
    for (auto& t : m_Workers)
        if (t.joinable()) t.join();
}

