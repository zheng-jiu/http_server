#pragma once

#include "tiny_http/async_logger.h"
#include "tiny_http/config.h"
#include "tiny_http/http_parser.h"
#include "tiny_http/http_response.h"
#include "tiny_http/mime_types.h"
#include "tiny_http/thread_pool.h"

#include <sys/types.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

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

        // GET 静态文件时，由 worker 打开文件并将所有权交给 I/O 线程
        int file_fd {-1};
        std::size_t file_size {0};
        
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

        int file_fd {-1};
        off_t file_offset {0};
        std::size_t file_remaining {0};

        bool close_after_write {true};
        bool processing {false};
        bool rejecting {false};
        long long last_active_ms {0};
        long long last_write_ms {0};
        int upstream_fd {-1}; // 当前关联的后端 Socket
    };

    // 一次代理请求对应的后端连接
    struct Upstream {
        int fd {-1};
        int client_fd {-1};
        std::uint64_t client_id {0};
        std::string backend_url;  // 本次请求选中的后端地址
        bool connecting {true};
        bool head_request {false};
        bool reused {false};       // 当前 Socket 来自连接池
        bool keep_alive {false};   // 后端响应允许保持连接

        std::string out;            // 待发给后端的请求
        std::size_t sent {0};       // 已发送的请求字节数
        std::string in;             // 后端响应原始字节

        std::size_t header_size {0};   // 0 表示响应头尚未解析
        std::optional<std::size_t> content_length;
        bool no_body {false};
        int status {0};
        std::string response_head;    // 整理后的状态行和普通响应头

        long long started_ms {0};
    };

    // 池里只保存空闲 Socket，使用中的 Socket 仍由 Upstream 管理
    struct IdleUpstream {
        int fd;
        long long idle_since_ms;
    };

    struct BackendPool {
        std::vector<IdleUpstream> idle;
        std::size_t busy {0}; // 正在连接或处理请求的 Socket 数量
    };

    static constexpr std::size_t proxy_header_limit_ = 8192;
    static constexpr std::size_t proxy_body_limit_ = 8 * 1024 * 1024;
    static constexpr long long proxy_timeout_ms_ = 15000;
    static constexpr long long backend_cooldown_ms_ = 10000;
    static constexpr std::size_t backend_connection_limit_ = 16;
    static constexpr long long backend_idle_timeout_ms_ = 15000;

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

    const LocationRule* match_location(const std::string& target) const;

    const std::string* select_backend(const LocationRule& location);

    void mark_backend_failed(const std::string& url);

    void start_proxy(
        int client_fd,
        const HttpRequest& request,
        const LocationRule& location
    );

    void handle_upstream(int fd, std::uint32_t flags);

    bool parse_upstream_headers(Upstream& upstream);

    void finish_proxy(int upstream_fd);

    void fail_proxy(
        int client_fd,
        int status,
        const std::string& reason,
        bool backend_failure = false
    );

    int take_idle_upstream(BackendPool& pool);
    void release_upstream(int fd, bool reusable);
    void close_upstream(int fd);
    void discard_idle_upstreams(BackendPool& pool);
    void sweep_idle_upstreams(long long now);

private:
    // ---- 私有变量 ----

    ServerConfig config_;

    int listen_fd_ {-1};
    int epoll_fd_ {-1};
    int completion_fd_ {-1};
    int signal_fd_ {-1};

    bool running_ {false};

    std::unordered_map<int, Connection> connections_;
    std::unordered_map<int, Upstream> upstreams_;
    std::unordered_map<std::string, BackendPool> backend_pools_;

    // 每条 location 规则下一次轮询的位置
    std::unordered_map<std::string, std::size_t> next_backend_;

    // 后端地址对应的冷却截止时间（毫秒）
    std::unordered_map<std::string, long long> backend_retry_at_;

    std::uint64_t next_connection_id_ {1};

    BlockingQueue<ResponseResult> completion_queue_;

    HttpParser parser_ {config_.max_body_size};
    ThreadPool thread_pool_ {config_.worker_threads};
    AsyncLogger logger_;
};

} // namespace tiny_http