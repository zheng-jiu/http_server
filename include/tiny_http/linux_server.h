#pragma once 

#include "tiny_http/async_logger.h"
#include "tiny_http/config.h"
#include "tiny_http/http_parser.h"
#include "tiny_http/http_response.h"
#include "tiny_http/mime_types.h"
#include "tiny_http/thread_pool.h"
#include <string>
#include <unordered_map>

namespace tiny_http {

class LinuxHttpServer {
public:
    explicit LinuxHttpServer(ServerConfig config);
    ~LinuxHttpServer();

    bool start();
    void stop();

    void setup_listener();
    void setup_epoll();

    // 事件处理
    void accept_clients();
    void handle_read(int fd);
    void handle_write(int fd);
    void sweep_idle_connections();

    // 辅助函数
    void close_connection(int fd);
    void process_request(int fd, const std::string& raw);
    std::string map_target_to_path(const std::string& target);
    void switch_to_epollout(int fd);
    void switch_to_epollin(int fd);

private:
    // 每个客户端连接的状态
    struct Connection {
        int fd {-1};
        std::string in;
        std::string out;
        bool close_after_write {true};
        bool processing {false};
        long long last_active_ms {0};
    };

    ServerConfig config_;
    int listen_fd_ {-1};
    int epoll_fd_ {-1};
    bool running_ {false};
    std::unordered_map<int, Connection> connections_;
    HttpParser parser_;
    ThreadPool thread_pool_ {config_.worker_threads};
    AsyncLogger logger_;
};

} // namespace tiny_http
