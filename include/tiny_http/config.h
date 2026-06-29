#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tiny_http {

// ---- 一个 location 规则 ----
// 例如：location /api/ { proxy_pass http://localhost:5000; }
struct LocationRule {
    std::string path; // 匹配路径，如 "/api/"
    std::string root; // 可选：替代 document_root
    std::string proxy_pass; // 可选：反向代理目标 URL
    std::string expires; // 可选：缓存过期时间，如 "7d"、"1h"
};

// ---- 一个虚拟主机 ----
// server_name "example.com" { root /var/www/example; }
struct VirtualHost {
    std::string server_name; // 域名
    std::string root; // 该虚拟主机的根目录
};

struct ServerConfig {
    int port {8080}; // 监听端口
    std::string document_root {"www"}; // 文档根目录
    std::size_t worker_threads {4}; // 工作线程数
    int keep_alive_seconds {15}; // keep-alive 超时时间（秒）
    int max_events {1024}; // epoll 最大事件数
    std::string log_path {"tiny_httpd.log"}; // 日志文件路径
    std::string log_level {"info"}; // debug / info / warn / error
    std::string config_file; // 配置文件路径（记录用）
    std::vector<VirtualHost> virtual_hosts; // 虚拟主机列表
    std::vector<LocationRule> locations; // location 规则列表
    std::size_t max_body_size {1048576}; // 最大请求体大小（默认 1MB)
    int rate_limit {0}; // 每秒最大请求数（0 = 不限制）
};

} // namespace tiny_http