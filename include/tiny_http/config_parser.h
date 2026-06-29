#pragma once

#include "tiny_http/config.h"
#include <string>

namespace tiny_http {

// 解析配置文件，返回填充好的 ServerConfig
// 解析失败时抛出 std::runtime_error(包含行号和错误描述)
ServerConfig parse_config_file(const std::string& file_path);

// 将命令行参数覆盖到 config （命令行优先级更高）
// 用法：config.port 默认 8080，但 ./tiny_httpd 9090 会覆盖为 9090
void apply_command_line(ServerConfig& config, int argc, char* argv[]);

} // namespace tiny_http