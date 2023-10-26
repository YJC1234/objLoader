#pragma once
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

using concurrency_t =
    std::invoke_result_t<decltype(std::thread::hardware_concurrency)>;

class ThreadPool {
private:
    std::queue<std::function<void()>> m_tasks = {};
    std::vector<std::thread>          m_threads = {};
    std::condition_variable           m_tasks_available_cv = {};
    std::condition_variable           m_tasks_done_cv = {};
    std::mutex                        m_mutex = {};
    bool                              m_workers_running = false;
    bool                              m_waiting = false;
    concurrency_t                     m_thread_count = 0;
    size_t                            m_tasks_running = 0;

    concurrency_t
    determine_thread_count(const concurrency_t thread_count) const {
        if (thread_count > 0) {
            return thread_count;
        } else {
            if (std::thread::hardware_concurrency() > 0) {
                return std::thread::hardware_concurrency();
            } else {
                return 1;
            }
        }
    }

    void worker() {
        std::function<void()> task;
        while (true) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_tasks_available_cv.wait(lock, [this] {
                return !m_workers_running || !m_tasks.empty();
            });
            if (!m_workers_running)
                break;
            task = std::move(m_tasks.front());
            m_tasks.pop();
            ++m_tasks_running;
            lock.unlock();
            task();
            lock.lock();
            --m_tasks_running;
            if (m_waiting && !m_tasks_running && m_tasks.empty()) {
                m_tasks_done_cv.notify_all();
            }
        }
    }
    void wait_for_tasks() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_waiting = true;
        m_tasks_done_cv.wait(
            lock, [this] { return !m_tasks_running && m_tasks.empty(); });
        m_waiting = false;
    }
    void destroy_threads() {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workers_running = false;
        }
        m_tasks_available_cv.notify_all();
        for (concurrency_t i = 0; i < m_thread_count; ++i) {
            m_threads[i].join();
        }
    }

public:
    ThreadPool(const concurrency_t thread_count = 0)
        : m_thread_count(determine_thread_count(thread_count)) {
        m_threads.resize(m_thread_count);
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workers_running = true;
        }
        for (concurrency_t i = 0; i < m_thread_count; ++i) {
            m_threads[i] = std::thread(&ThreadPool::worker, this);
        }
    }
    ~ThreadPool() {
        wait_for_tasks();
        destroy_threads();
    }

    template <class F, class... A>
    void push_task(F&& task, A&&... args) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_tasks.push(
                std::bind(std::forward<F>(task), std::forward<A>(args)...));
        }
        m_tasks_available_cv.notify_one();
    }

    auto schedule() {
        struct Awaiter : public std::suspend_always {
            ThreadPool& pool;
            Awaiter(ThreadPool& pool_) : pool(pool_) {}

            void await_suspend(std::coroutine_handle<> handle) {
                pool.push_task([handle] { handle.resume(); });
            }
        };
        return Awaiter(*this);
    }
};