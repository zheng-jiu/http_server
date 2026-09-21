#pragma once

#include "tiny_http/async_logger.h"
#include "tiny_http/config.h"
#include "tiny_http/http_parser.h"
#include "tiny_http/http_response.h"
#include "tiny_http/mime_types.h"
#include "tiny_http/thread_pool.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace tiny_http {

class LinuxHttpServer {
public:
    // ---- 公有函数 ----

    explicit LinuxHttpServer(ServerConfig config);
    ~LinuxHttpServer();

    bool start();
    void stop();

private:
    // ---- 私有类型 ----

    // worker 处理完成后返回给 I/O 线程的结果
    struct ResponseResult {
        int fd {-1};
        std::uint64_t connection_id {0};

        std::string response;
        bool close_after_write {true};

        // 不发送响应，直接由 I/O 线程关闭该连接
        bool close_connection {false};
    };

    // 每个客户端连接的状态
    struct Connection {
        int fd {-1};
        std::uint64_t id {0};

        std::string in;
        std::string out;

        bool close_after_write {true};
        bool processing {false};
        long long last_active_ms {0};
    };

private:
    // ---- 私有函数 ----

    // 初始化
    void setup_listener();
    void setup_epoll();

    // 事件处理
    void accept_clients();
    void handle_read(int fd);
    void handle_write(int fd);
    void handle_completions();
    void sweep_idle_connections();

    // 请求处理
    ResponseResult process_request(
        int fd,
        std::uint64_t connection_id,
        const std::string& raw
    );

    // 辅助函数
    bool try_submit_next_request(int fd);

    void close_connection(int fd);

    std::string map_target_to_path(
        const std::string& target
    );

    void switch_to_epollout(int fd);
    void switch_to_epollin(int fd);

private:
    // ---- 私有变量 ----

    ServerConfig config_;

    int listen_fd_ {-1};
    int epoll_fd_ {-1};
    int completion_fd_ {-1};

    bool running_ {false};

    std::unordered_map<int, Connection> connections_;
    std::uint64_t next_connection_id_ {1};

    BlockingQueue<ResponseResult> completion_queue_;

    HttpParser parser_;
    ThreadPool thread_pool_ {config_.worker_threads};
    AsyncLogger logger_;
};

} // namespace tiny_http