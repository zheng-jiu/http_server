#include "tiny_http/linux_server.h"
#include "tiny_http/config_parser.h"

#include <csignal>
#include <iostream>
#include <cstdlib>
#include <string>

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

    // ========================================================
    // 第1步：尝试加载配置文件
    // ========================================================
    std::string config_path = "tiny_httpd.conf";

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-c" && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    try {
        config = tiny_http::parse_config_file(config_path);
        std::cout << "[配置] 已加载配置文件：" << config_path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[配置] 警告：" << e.what() << "\n";
        std::cerr << "[配置] 将使用默认配置启动\n";
    }

    // ========================================================
    // 第2步：命令行参数覆盖（优先级高于配置文件）
    // ========================================================
    tiny_http::apply_command_line(config, argc, argv);

    // ========================================================
    // 第3步：打印最终生效的配置
    // ========================================================
    std::cout << "========================================\n";
    std::cout << "Tiny HTTP Server 启动配置:\n";
    std::cout << "  port           = " << config.port << "\n";
    std::cout << "  document_root  = " << config.document_root << "\n";
    std::cout << "  worker_threads = " << config.worker_threads << "\n";
    std::cout << "  keep_alive     = " << config.keep_alive_seconds << "s\n";
    std::cout << "  max_events     = " << config.max_events << "\n";
    std::cout << "  log_path       = " << config.log_path << "\n";
    std::cout << "  log_level      = " << config.log_level << "\n";
    std::cout << "  max_body_size  = " << config.max_body_size << " bytes\n";
    if (config.rate_limit > 0) {
        std::cout << "  rate_limit     = " << config.rate_limit << " req/s\n";
    }
    if (!config.virtual_hosts.empty()) {
        std::cout << "  virtual_hosts  = "
                  << config.virtual_hosts.size() << " 个\n";
    }
    if (!config.locations.empty()) {
        std::cout << "  location       = "
                  << config.locations.size() << " 个\n";
    }
    std::cout << "========================================\n";

    // ========================================================
    // 第4步：启动服务器
    // ========================================================
    tiny_http::LinuxHttpServer server(config);
    g_server = &server;

    if (!server.start()) {
        std::cerr << "启动失败！\n";
        return 1;
    }

    std::cout << "服务器已正常退出\n";
    return 0;
}