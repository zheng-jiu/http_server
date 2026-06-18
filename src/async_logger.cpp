#include "tiny_http/async_logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace tiny_http {

// ---- 辅助：时间戳 ----

std::string AsyncLogger::timestamp()
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const time_t t = std:: chrono::system_clock::to_time_t(now);
    const std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm tm{};
    ::localtime_r(&t, &tm);  // 线程安全版本

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// ---- 辅助：级别转字符串 ----

const char* AsyncLogger::level_string(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "???";
}

// ---- 辅助：格式化一条日志 ----

std::string AsyncLogger::format(LogLevel level, const std::string& message)
{
    std::ostringstream oss;
    oss << "[" << timestamp() << "]"
        << " [" << level_string(level) << "] "
        << message;
    return oss.str();
}

// ---- 构造与析构 ----

AsyncLogger::AsyncLogger(const std::string& file_path, LogLevel min_level)
    : file_path_(file_path), min_level_(min_level) {}

AsyncLogger::~AsyncLogger()
{
    stop();
}

// ---- 启动 & 停止 ----

void AsyncLogger::start()
{
    if (running_.exchange(true)) {
        return;  // 已经在运行
    }
    output_.open(file_path_, std::ios::app);
    if (!output_) {
        std::cerr << "AsyncLogger: 无法打开 " << file_path_ << "\n";
        running_ = false;
        return;
    }
    worker_ = std::thread(&AsyncLogger::worker_loop, this);
}

void AsyncLogger::stop()
{
    if (!running_.exchange(false)) {
        return;  // 已经停止
    }
    cv_.notify_all();  // 唤醒后台线程
    if (worker_.joinable()) {
        worker_.join();
    }
    if (output_.is_open()) {
        output_.flush();
        output_.close();
    }
}

// ---- 记录日志（业务线程调用）----

void AsyncLogger::log(LogLevel level, const std::string& message)
{
    // 1.级别过滤：低于最小级别的丢弃
    if (static_cast<int>(level) < static_cast<int>(min_level_)) {
        return;
    }
    // 2.加锁入队
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;  // 日志器已停止
        }
        queue_.push_back(format(level, message));
    }   // 离开作用域，自动释放锁
    // 3.唤醒后台线程
    cv_.notify_one();
}

// ---- 后台线程主循环 ----

void AsyncLogger::worker_loop()
{
    std::vector<std::string> batch;
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_ || !queue_.empty();
            });
            if (!running_ && queue_.empty()) {
                break;
            }
            batch.swap(queue_);  // 批量交换，减少持锁时间
        }
        // 无锁写文件
        for (const std::string& line : batch) {
            output_ << line << '\n';
        }
        output_.flush();  // 每次批量写后刷新到磁盘
        batch.clear();
    }
    // 退出前清空残留日志
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(queue_);
    }
    for (const std::string& line : batch) {
        output_ << line << '\n';
    }
    output_.flush();
}

} // namespace tiny_http