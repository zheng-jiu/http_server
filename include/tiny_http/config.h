#pragma once

#include <cstddef>
#include <string>

namespace tiny_http {

struct ServerConfig {
    int port {8080};
    std::string document_root {"www"};
    std::size_t worker_threads {4};
    int keep_alive_seconds {15};
    int max_events {1024};
    std::string log_path {"tiny_httpd.log"};
};

} // namespace tiny_http