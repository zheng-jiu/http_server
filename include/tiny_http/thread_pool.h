#pragma once

#include "tiny_http/blocking_queue.h"

#include <functional>
#include <thread>
#include <vector>

namespace tiny_http {

class ThreadPool {
public:
    // 等价于 typedef std::function<void()> Task;
    using Task = std::function<void()>;

    explicit ThreadPool(std::size_t num_threads);
    ~ThreadPool();

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务
    void submit(Task task);

    // 停止所有线程
    void stop();

private:
    void worker_loop();  // 每个工作线程的主循环

    BlockingQueue<Task> tasks_;
    std::vector<std::thread> workers_;
};

} // namespace tiny_http