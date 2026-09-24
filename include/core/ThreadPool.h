#pragma once
#include <vector>
#include <thread>
#include <future>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <type_traits>
#include <utility>

class ThreadPool {
public:
    explicit ThreadPool(size_t n = std::thread::hardware_concurrency());
    ~ThreadPool();

    template<typename F, typename... Args>
    auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>;

    void Shutdown();

    /// Process-wide pool. First call wins the size; later calls return the same pool.
    static ThreadPool& Shared(size_t n = 0);

private:
    std::vector<std::thread> m_Workers;
    std::queue<std::function<void()>> m_Tasks;
    std::mutex m_Mtx;
    std::condition_variable m_Cv;
    bool m_Stopping = false;
};

template<typename F, typename... Args>
auto ThreadPool::Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
    using R = decltype(f(args...));
    auto task = std::make_shared<std::packaged_task<R()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<R> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(m_Mtx);
        if (m_Stopping) throw std::runtime_error("submit on stopped ThreadPool");
        m_Tasks.emplace([task]() { (*task)(); });
    }
    m_Cv.notify_one();
    return res;
}
