#include "tiny_http/linux_server.h"

#include <csignal>
#include <iostream>
#include <cstdlib>

// 全局服务器指针（信号处理函数需要访问）
tiny_http::LinuxHttpServer* g_server = nullptr;

// 信号处理函数
void handle_signal(int /*signum*/)
{
    if (g_server != nullptr) {
        g_server->stop(); // 触发优雅关闭
    }
}

int main(int argc, char* argv[])
{
    // 注册信号处理
    std::signal(SIGINT, handle_signal);  // Ctrl+C
    std::signal(SIGTERM, handle_signal);  // kill 命令

    tiny_http::ServerConfig config;

    // 命令行参数：[port] [document_root] [worker_threads]
    if (argc >= 2) {
        config.port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        config.document_root = argv[2];
    }
    if (argc >= 4) {
        config.worker_threads = static_cast<std::size_t>(std::atoi(argv[3]));
    }

    std::cout << "配置: port=" << config.port
              << " root=" << config.document_root
              << " workers=" << config.worker_threads << "\n";

    tiny_http::LinuxHttpServer server(config);
    g_server = &server;

    if (!server.start()) {
        std::cerr << "启动失败！\n";
        return 1;
    }

    std::cout << "服务器已正常退出\n";
    return 0;
}