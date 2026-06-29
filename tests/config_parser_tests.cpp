#include "tiny_http/config_parser.h"

#include <cassert>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

// 辅助函数：将字符串写入临时文件，返回文件路径
static std::string write_temp_config(const std::string& content) {
    std::string path = "/tmp/tiny_httpd_test_" + std::to_string(std::rand()) + ".conf"; 
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// 辅助函数：从字符串直接解析配置（不用手动创建文件） "写入临时文件 → 解析 → 删除临时文件"
static tiny_http::ServerConfig parse_string(const std::string& content) {
    std::string path = write_temp_config(content);
    tiny_http::ServerConfig config = tiny_http::parse_config_file(path);
    std::remove(path.c_str());  // 清理临时文件
    return config;
}

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    int passed = 0;
    int failed = 0;

    // 辅助 lambda：检查条件并打印结果
    auto check = [&](bool condition, const std::string& name) {
        if (condition) {
            std::cout << "  [PASS] " << name << "\n";
            passed++;
        } else {
            std::cerr << "  [FAIL] " << name << "\n";
            failed++;
        }
    };

    // ========================================================
    // 测试组1：基本指令解析（9个指令全部测试）
    // ========================================================
    std::cout << "--- 测试组1: 基本指令 ---\n";

    {
        tiny_http::ServerConfig config = parse_string(
            "server {\n"
            "    listen 9090;\n"
            "    root \"/tmp/test_www\";\n"
            "    worker_threads 8;\n"
            "    keep_alive 30;\n"
            "    max_events 2048;\n"
            "    log_path \"/var/log/tiny.log\";\n"
            "    log_level debug;\n"
            "    max_body_size 2097152;\n"
            "    rate_limit 100;\n"
            "}\n"
        );

        check(config.port == 9090,                     "port = 9090");
        check(config.document_root == "/tmp/test_www", "root = /tmp/test_www");
        check(config.worker_threads == 8,              "worker_threads = 8");
        check(config.keep_alive_seconds == 30,         "keep_alive = 30");
        check(config.max_events == 2048,               "max_events = 2048");
        check(config.log_path == "/var/log/tiny.log",  "log_path");
        check(config.log_level == "debug",             "log_level = debug");
        check(config.max_body_size == 2097152,         "max_body_size = 2MB");
        check(config.rate_limit == 100,                "rate_limit = 100");
    }

    // ========================================================
    // 测试组2：location 块（测试嵌套解析）
    // ========================================================
    std::cout << "--- 测试组2: location 块 ---\n";

    {
        tiny_http::ServerConfig config = parse_string(
            "server {\n"
            "    listen 8080;\n"
            "    location /api/ {\n"
            "        proxy_pass \"http://localhost:5000\";\n"
            "    }\n"
            "    location /static/ {\n"
            "        root \"/srv/static\";\n"
            "        expires 7d;\n"
            "    }\n"
            "}\n"
        );

        check(config.locations.size() == 2, "2 个location");

        if (config.locations.size() >= 2) {
            check(config.locations[0].path == "/api/",
                  "location[0] path = /api/");
            check(config.locations[0].proxy_pass == "http://localhost:5000",
                  "location[0] proxy_pass");
            check(config.locations[1].path == "/static/",
                  "location[1] path = /static/");
            check(config.locations[1].root == "/srv/static",
                  "location[1] root");
            check(config.locations[1].expires == "7d",
                  "location[1] expires = 7d");
        }
    }

    // ========================================================
    // 测试组3：虚拟主机（server_name）
    // ========================================================
    std::cout << "--- 测试组3: 虚拟主机 ---\n";

    {
        tiny_http::ServerConfig config = parse_string(
            "server {\n"
            "    listen 8080;\n"
            "    server_name example.com {\n"
            "        root \"/var/www/example\";\n"
            "    }\n"\
            "    server_name \"blog.example.com\" {\n"
            "        root \"/var/www/blog\";\n"
            "    }\n"
            "}\n"
        );

        check(config.virtual_hosts.size() == 2, "2 个虚拟主机");

        if (config.virtual_hosts.size() >= 2) {
            check(config.virtual_hosts[0].server_name == "example.com",
                  "vh[0] name = example.com");
            check(config.virtual_hosts[0].root == "/var/www/example",
                  "vh[0] root");
            check(config.virtual_hosts[1].server_name == "blog.example.com",
                  "vh[1] name = blog.example.com");
            check(config.virtual_hosts[1].root == "/var/www/blog",
                  "vh[1] root");
        }
    }

    // ========================================================
    // 测试组4：注释处理（单行注释 + 行尾注释）
    // ========================================================
    std::cout << "--- 测试组4: 注释 ---\n";

    {
        tiny_http::ServerConfig config = parse_string(
            "# 这是注释行\n"
            "server {\n"
            "    # 端口号\n"
            "    listen 1234;   # 行尾注释\n"
            "    root \"/www\";  # 文档根目录\n"
            "}\n"
        );

        check(config.port == 1234,  "注释行被正确跳过");
        check(config.document_root == "/www", "注释不影响指令解析");
    }

    // ========================================================
    // 测试组5：空配置（应返回默认值）
    // ========================================================
    std::cout << "--- 测试组5: 空配置 ---\n";

    {
        tiny_http::ServerConfig config = parse_string("");

        check(config.port == 8080,           "空配置 port 保持默认");
        check(config.document_root == "www", "空配置 root 保持默认");
        check(config.worker_threads == 4,    "空配置 workers 保持默认");
        check(config.locations.empty(),      "空配置无 location");
        check(config.virtual_hosts.empty(),  "空配置无虚拟主机");
    }

    // ========================================================
    // 测试组6：错误处理（未知指令 + 缺少分号）
    // ========================================================
    std::cout << "--- 测试组6: 错误处理 ---\n";

    {
        bool caught = false;
        try {
            parse_string("server { unknown_directive 123; }\n");
        } catch (const std::runtime_error& e) {
            caught = true;
            std::cout << "    预期错误: " << e.what() << "\n";
        }
        check(caught, "未知指令应抛出异常");
        
        caught = false;
        try {
            parse_string("server { listen 8080 }\n");  // 缺少分号
        } catch (const std::runtime_error& e) {
            caught = true;
            std::cout << "    预期错误: " << e.what() << "\n";
        }
        check(caught, "缺少分号应抛出异常");
    }

    // ========================================================
    // 测试组7：命令行参数覆盖
    // ========================================================
    std::cout << "--- 测试组7: 命令行覆盖 ---\n";

    {
        tiny_http::ServerConfig config;
        config.port = 8080;
        config.document_root = "www";
        config.worker_threads = 4;

        // 模拟：./tiny_httpd 9090 /var/www 8
        const char* argv[] = {"tiny_httpd", "9090", "/var/www", "8"};
        tiny_http::apply_command_line(config, 4, const_cast<char**>(argv));

        check(config.port == 9090,                  "命令行覆盖 port");
        check(config.document_root == "/var/www",   "命令行覆盖 root");
        check(config.worker_threads == 8,           "命令行覆盖 workers");
    }

    // ========================================================
    // 测试组8：配置文件不存在时抛异常
    // ========================================================
    std::cout << "--- 测试组8: 配置文件不存在 ---\n";

    {
        bool caught = false;
        try {
            tiny_http::parse_config_file("/tmp/nonexistent_xyz.conf");
        } catch (const std::runtime_error& e) {
            caught = true;
            std::cout << "    预期错误: " << e.what() << "\n";
        }
        check(caught, "不存在的配置文件应抛出异常");
    }

    // ========================================================
    // 结果汇总 
    // ========================================================
    std::cout << "\n========================================\n";
    std::cout << "配置解析器测试结果: "
              << passed << " 通过, " << failed << " 失败\n";
    std::cout << "========================================\n";

    return failed > 0 ? 1 : 0;
}