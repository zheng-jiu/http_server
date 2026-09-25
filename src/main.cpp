#include "tiny_http/linux_server.h"
#include "tiny_http/config_parser.h"

#include <csignal>
#include <iostream>
#include <cstdlib>
#include <string>
#include <signal.h>
#include <cerrno>
#include <pthread.h>

int main(int argc, char* argv[])
{
    // 忽略 SIGPIPE，让发送失败通过返回值报告
    // 避免单个连接断开导致整个服务器进程退出
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    ::sigemptyset(&action.sa_mask);

    if (::sigaction(SIGPIPE, &action, nullptr) < 0) {
        std::cerr << "sigaction(SIGPIPE) failed, errno=" << errno << "\n";
        return 1;
    }

    // 屏蔽退出信号，后续由 I/O 线程通过 signalfd 接收
    sigset_t stop_signals {};
    ::sigemptyset(&stop_signals);
    ::sigaddset(&stop_signals, SIGINT);
    ::sigaddset(&stop_signals, SIGTERM);

    const int error = ::pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    if (error != 0) {
        // pthread_sigmask 直接返回错误码，不是通过 errno 报告
        std::cerr << "pthread_sigmask() failed, error=" << error << "\n";
        return 1;
    }

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

    if (!server.start()) {
        std::cerr << "启动失败！\n";
        return 1;
    }

    std::cout << "服务器已正常退出\n";
    return 0;
}