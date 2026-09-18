#include "tiny_http/config_parser.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tiny_http {

// ============================================================
// 第1部分：Token 定义 —— 词法分析的基本单位
// ============================================================

// Token 的类型：配置文件由这些基本元素组成
enum class TokenType {
    Keyword,    // 如 listen, server, location, root
    Number,     // 如 8080, 15
    String,     // 如 "example.com"（双引号包裹）
    Path,       // 如 /api/（以 / 开头）
    Semicolon,  // ;
    LBrace,     // {
    RBrace,     // }
    Eof         // 文件结束
};

// 一个 Token 对象：类型 + 内容 + 所在行号
struct Token {
    TokenType type;
    std::string value;
    int line {0};  // 行号，用于报错时定位
};

// ============================================================
// 第2部分：词法分析器（Lexer）
// 职责：把原始字符串切成 Token 序列
// ============================================================

class Lexer {
public:
    // 构造函数：接收整个配置文件的文本内容
    explicit Lexer(std::string input)
        : input_(std::move(input)), pos_(0), line_(1) {}

    // 读取下一个 Token（调用一次返回一个）
    Token next() {
        skip_whitespace_and_comments();  // 跳过空白和注释

        // 文件结束了
        if (pos_ >= input_.size()) {
            return {TokenType::Eof, "", line_};
        }

        char c = input_[pos_];

        // ---- 单字符 Token：直接返回 ----
        if (c == ';') {
            pos_++;
            return {TokenType::Semicolon, ";", line_};
        }
        if (c == '{') {
            pos_++;
            return {TokenType::LBrace, "{", line_};
        }
        if (c == '}') {
            pos_++;
            return {TokenType::RBrace, "}", line_};
        }

        // ---- 复杂 Token：交给专门的扫描函数 ----
        if (c == '"') {
            return scan_string();  // "example.com"
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            return scan_number();  // 8080
        }
        if (c == '/') {
            return scan_path();  // /api/
        }

        // 剩下的就是关键字（如 listen, server)
        return scan_keyword();
    }

private:
    // ========== 辅助函数 ==========

    // 跳过空格、制表符、换行、注释（# 开头到行尾）
    void skip_whitespace_and_comments() {
        while (pos_ < input_.size()) {
            char c = input_[pos_];

            if (c == '\n') {
                line_++;  // 遇到换行，行号加1
                pos_++;
            } else if (c == '#') {
                // 遇到 # 号，跳过整行（注释）
                while (pos_ < input_.size() && input_[pos_] != '\n') {
                    pos_++;
                }
                // 注意：不在这里 line_++，因为换行符 \n 会在下一轮处理
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                pos_++;  // 跳过普通空白
            } else {
                break;   // 遇到有效字符，停止
            }
        }
    }

    // 扫描引号包裹的字符串："hello world" -> hello world
    Token scan_string() {
        pos_++;  // 跳过开始的 "
        std::string value;
        while (pos_ < input_.size() && input_[pos_] != '"') {
            // 简单处理转义：\" 表示引号本身
            if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
                pos_++;
            }
            value += input_[pos_];
            pos_++;
        }
        if (pos_ < input_.size()) {
            pos_++;  // 跳过结束的 "
        }
        return {TokenType::String, value, line_};
    }

    // 扫描数字：8080 -> "8080"
    // 如果数字后紧跟字母（如 7d、30m），整体作为关键字"7d"返回
    Token scan_number() {
        std::string value;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            value += input_[pos_];
            pos_++;
        }
        // 数字后紧跟字母 → 这是复合值（如 7d），继续吃掉，按关键字返回
        if (pos_ < input_.size() && std::isalpha(static_cast<unsigned char>(input_[pos_]))) {
            while (pos_ < input_.size()
                   && !std::isspace(static_cast<unsigned char>(input_[pos_]))
                   && input_[pos_] != ';'
                   && input_[pos_] != '{'
                   && input_[pos_] != '}') {
                value += input_[pos_];
                pos_++;
            }
            return {TokenType::Keyword, value, line_};
        }
        return {TokenType::Number, value, line_};
    }

    // 扫描路径：/api/v1/users -> "/api/v1/users"
    Token scan_path() {
        std::string value;
        while (pos_ < input_.size() 
                && !std::isspace(static_cast<unsigned char>(input_[pos_]))
                && input_[pos_] != ';'
                && input_[pos_] != '{') {
            value += input_[pos_];
            pos_++;
        }
        return {TokenType::Path, value, line_};
    }

    // 扫描关键字/标识符：listen -> "listen"
    Token scan_keyword() {
        std::string value;
        while (pos_ < input_.size()
                && !std::isspace(static_cast<unsigned char>(input_[pos_]))
                && input_[pos_] != ';'
                && input_[pos_] != '{'
                && input_[pos_] != '}') {
            value += input_[pos_];
            pos_++;
        }
        return {TokenType::Keyword, value, line_};
    }

    // ========== 成员变量 ==========
    std::string input_;  // 整个配置文件的内容
    size_t pos_;         // 当前读取位置（索引）
    int line_;           // 当前行号（用于错误提示）
};

// ============================================================
// 第3部分：语法分析器（Parser）
// 职责：把 Token 序列转换成 ServerConfig 结构体
// 采用"递归下降"算法——每个语法结构对应一个解析函数
// ============================================================

class Parser {
public:
    explicit Parser(std::string input)
        : lexer_(std::move(input))
    {
        current_ = lexer_.next(); // 预读第一个 Token
    }

    // 入口：解析整个配置文件
    ServerConfig parse() {
        ServerConfig config;
        // 循环解析顶层结构，直到文件结束
        while (current_.type != TokenType::Eof) {
            parse_top_level(config);
        }
        
        return config;
    }

private:
    // ========== 顶层分发 ==========
    
    // 顶层只能出现两种东西
    //   1. server { ... } 块
    //   2. 独立指令（如 listen 8080;)
    void parse_top_level(ServerConfig& config) {
        if (current_.type == TokenType::Keyword && current_.value == "server") {
            parse_server_block(config);
        } else if (current_.type == TokenType::Keyword) {
            parse_directive(config);
        } else {
            error("顶层只能出现 server 块或指令，但遇到了：" + current_.value);
        }
    }

    // ========== 块结构 ==========

    // server { ... } 块
    // 内部可以包含：server_name 块、location 块、普通指令
    void parse_server_block(ServerConfig& config) {
        expect(TokenType::Keyword);  // 消费 "server"
        expect(TokenType::LBrace);  // 消费 "{"

        // 循环处理块内部，直到遇到 }
        while (current_.type != TokenType::RBrace && current_.type != TokenType::Eof) {
            
            if (current_.type == TokenType::Keyword && current_.value == "server_name") {
                parse_virtual_host(config);
            } else if (current_.type == TokenType::Keyword && current_.value == "location") {
                parse_location(config);
            } else if (current_.type == TokenType::Keyword) {
                parse_directive(config);
            } else {
                error ("server 块内出现了意外的 token: " + current_.value);
            }
        }

        expect(TokenType::RBrace);  // 消费 "}"
    }

    // server_name "example.com" { root "/var/www"; }
    void parse_virtual_host(ServerConfig& config) {
        expect(TokenType::Keyword);  // 消费 "server_name"
        VirtualHost vh;
        vh.server_name = current_.value;
        // 域名可以是 String 或 Keyword（不带引号的也接受）
        if (current_.type == TokenType::String) {
            expect(TokenType::String);
        } else {
            expect(TokenType::Keyword);
        }

        expect(TokenType::LBrace);  // 消费 "{"

        // 处理块内部的指令
        while (current_.type != TokenType::RBrace && current_.type != TokenType::Eof) {
            if (current_.type == TokenType::Keyword && current_.value == "root") {
                expect(TokenType::Keyword);  // 消费 "root"
                vh.root = current_.value; 
                // root 的值可能是 String 或 Path
                if (current_.type == TokenType::String) {
                    expect(TokenType::String);
                } else if (current_.type == TokenType::Path) {
                    expect(TokenType::Path);
                } else {
                    error("root 指令后需要一个路径或字符串");
                }
                expect(TokenType::Semicolon);
            } else {
                error("server_name 块内不支持的指令：" + current_.value);
            }
        }
        
        expect(TokenType::RBrace);  // 消费 "}"
        config.virtual_hosts.push_back(std::move(vh));
    }

    // location /path/ { ... }
    void parse_location(ServerConfig& config) {
        expect(TokenType::Keyword);  // 消费 "location"
        
        LocationRule loc;
        loc.path = current_.value;  // 保存路径，如 "/api/"
        if (current_.type == TokenType::Path) {
            expect(TokenType::Path);
        } else if (current_.type == TokenType::String) {
            expect(TokenType::String);
        } else {
            error("location 后需要一个路径");
        }

        expect(TokenType::LBrace);  // 消费 "{"

        // 处理 location 块内部的指令
        while (current_.type != TokenType::RBrace && current_.type != TokenType::Eof) {
            if (current_.type == TokenType::Keyword && current_.value == "root") {
                expect(TokenType::Keyword);  // 消费 "root"
                loc.root = current_.value;
                if (current_.type == TokenType::String) {
                    expect(TokenType::String);
                } else if (current_.type == TokenType::Path) {
                    expect(TokenType::Path);
                } else {
                    error("root 指令后需要一个路径");
                }
                expect(TokenType::Semicolon);

            } else if (current_.type == TokenType::Keyword && current_.value == "proxy_pass") {
                expect(TokenType::Keyword);  // 消费 "proxy_pass"
                loc.proxy_pass = current_.value;
                if (current_.type == TokenType::String) {
                    expect(TokenType::String);
                } else {
                    expect(TokenType::Keyword);  // 不带引号的 URL
                }
                expect(TokenType::Semicolon);

            } else if (current_.type == TokenType::Keyword && current_.value == "expires") {
                expect(TokenType::Keyword);  // 消费 "expires"
                loc.expires = current_.value;
                expect(TokenType::Keyword);  // 如 "7d", "1h"
                expect(TokenType::Semicolon);

            } else {
                error("location 块内不支持的指令：" + current_.value);
            }
        }

        expect(TokenType::RBrace);  // 消费 "}"
        config.locations.push_back(std::move(loc));
    }

    // ========== 独立指令 ==========

    // 解析一条指令，如：listen 8080;
    // 根据指令名分发到对应的字段
    void parse_directive(ServerConfig& config) {
        std::string name = current_.value;
        expect(TokenType::Keyword);  // 消费指令名

        if (name == "listen") {
            config.port = std::stoi(current_.value);
            expect(TokenType::Number);

        } else if (name == "root") {
            config.document_root = current_.value;
            if (current_.type == TokenType::String) {
                expect(TokenType::String);
            } else if (current_.type == TokenType::Path) {
                expect(TokenType::Path);
            } else {
                error("root 需要一个路径参数");
            }

        } else if (name == "worker_threads") {
            config.worker_threads = static_cast<std::size_t>(std::stoul(current_.value));
            expect(TokenType::Number);

        } else if (name == "keep_alive") {
            config.keep_alive_seconds = std::stoi(current_.value);
            expect(TokenType::Number);

        } else if (name == "max_events") {
            config.max_events = std::stoi(current_.value);
            expect(TokenType::Number);

        } else if (name == "log_path") {
            config.log_path = current_.value;
            if (current_.type == TokenType::String) {
                expect(TokenType::String);
            } else {
                expect(TokenType::Keyword);  // 无引号的路径也接受
            }
            
        } else if (name == "log_level") {
            config.log_level = current_.value;
            expect(TokenType::Keyword);

        } else if (name == "max_body_size") {
            config.max_body_size = static_cast<std::size_t>(std::stoul(current_.value));
            expect(TokenType::Number);

        } else if (name == "rate_limit") {
            config.rate_limit = std::stoi(current_.value);
            expect(TokenType::Number);

        } else {
            // 遇到不认识的指令，报错（不默默忽略，帮助发现拼写错误）
            error("未知的配置指令：" + name);
        }

        // 每条指令必须以分号结尾
        expect(TokenType::Semicolon);
    }

    // ========== 辅助函数 ==========

    // 断言当前 Token 的类型，匹配则消费并读取下一个
    // 不匹配则抛出异常
    void expect(TokenType type) {
        if (current_.type != type) {
            error("期望 " + token_type_name(type) + "，但遇到了：" + current_.value);
        }
        current_ = lexer_.next();  // 前进到下一个 Token
    }

    // 拼一个人类可读的 Token 类型名（用于错误提示）
    static std::string token_type_name(TokenType t) {
        switch (t) {
            case TokenType::Keyword:    return "关键字";
            case TokenType::Number:     return "数字";
            case TokenType::String:     return "字符串";
            case TokenType::Path:       return "路径";
            case TokenType::Semicolon:  return "';'";
            case TokenType::LBrace:     return "'{'";
            case TokenType::RBrace:     return "'}'";
            case TokenType::Eof:        return "文件结尾";
            default:                    return "未知";
        }
    }

    // 报错：抛出包含行号的异常
    [[noreturn]] void error(const std::string& msg) {
        throw std::runtime_error("配置文件第 " + std::to_string(current_.line) + " 行：" + msg);
    }

    // ========== 成员变量 ==========
    Lexer lexer_;  // 词法分析器
    Token current_;  // 当前正在处理的 Token（lookahead)
};

// ============================================================
// 第4部分：公开接口
// ============================================================

// 从文件路径加载并解析配置
ServerConfig parse_config_file(const std::string& file_path) {
    // 1. 打开文件
    std::ifstream file(file_path);
    if (!file) {
        throw std::runtime_error("无法打开配置文件：" + file_path);
    }

    // 2. 读取整个文件到字符串
    std::ostringstream oss;
    oss << file.rdbuf();
    std::string content = oss.str();
    
    // 3. 检查空文件
    if (content.empty()) {
        // 空文件 = 使用默认配置，不报错
        return ServerConfig{};
    }

    // 4. 交给 Parser 解析
    Parser parser(std::move(content));
    ServerConfig config = parser.parse();
    config.config_file = file_path;  // 记录配置文件路径
    return config;
}

// 命令行参数覆盖配置值
void apply_command_line(ServerConfig& config, int argc, char* argv[]) {
    // 位置参数：[port] [document_root] [worker_threads]
    if (argc >= 2) {
        config.port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        config.document_root = argv[2];
    }
    if (argc >= 4) {
        config.worker_threads = static_cast<std::size_t>(std::atoi(argv[3]));
    }
}

} // namespace tiny_http