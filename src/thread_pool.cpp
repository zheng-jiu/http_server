#include "tiny_http/thread_pool.h"

namespace tiny_http {

ThreadPool::ThreadPool(std::size_t num_threads)
{
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::submit(Task task)
{
    tasks_.push(std::move(task));
}

void ThreadPool::stop()
{
    tasks_.close();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop()
{
    while (std::optional<Task> task = tasks_.pop()) {
        (*task)();  // 执行任务
    }
}


} // namespace tiny_http