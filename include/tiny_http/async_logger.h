#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tiny_http {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

class AsyncLogger {
public:
    explicit AsyncLogger(const std::string& file_path,
                         LogLevel min_level = LogLevel::Info);
    ~AsyncLogger();

    // 禁止拷贝
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // 启动后台写线程
    void start();
    // 停止后台线程
    void stop();

    // 记录日志（业务线程调用）
    void log(LogLevel level, const std::string& message);

private:
    void worker_loop();
    static std::string format(LogLevel level, const std::string& message);
    static std::string timestamp();
    static const char* level_string(LogLevel level);

    std::string file_path_;
    LogLevel min_level_;
    std::ofstream output_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> queue_;
    std::thread worker_;
    std::atomic<bool> running_ {false};
};

} // namespace tiny_http