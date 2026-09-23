#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace tiny_http {

template <typename T>
class BlockingQueue {
public:
    // 向队列添加任务
    void push(T value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;   // 队列已关闭，拒绝新任务
            }
            queue_.push(std::move(value));
        }   // 离开作用域，自动释放锁
        cv_.notify_one();  // 唤醒一个等待任务的工作线程
    }

    // 从队列取出任务（阻塞等待）
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return std::nullopt; // 队列为空且已关闭
        }

        T value = std::move(queue_.front());
        queue_.pop();
        
        return value; 
    }

    // 尝试从队列取出结果（非阻塞）
    std::optional<T> try_pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return std::nullopt; // 队列为空
        }

        T value = std::move(queue_.front());
        queue_.pop();

        return value;
    }

    // 关闭队列
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();  // 唤醒所有等待线程
    }

    bool is_closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_); 
        return closed_; 
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ {false};
};

} // namespace tiny_http