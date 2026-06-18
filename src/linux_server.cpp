#include "tiny_http/linux_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>

namespace tiny_http {

// ---- 构造与析构 ----   

LinuxHttpServer::LinuxHttpServer(ServerConfig config)
    : config_(std::move(config)), logger_(config_.log_path) {}

LinuxHttpServer::~LinuxHttpServer() 
{
    stop(); // RAII：对象销毁时自动释放所有资源
}

// ---- 辅助：获取当前毫秒时间戳 ----

static long long now_ms()
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

// ---- 辅助：设置非阻塞 ----

static bool set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ---- 监听 socket ----

void LinuxHttpServer::setup_listener()
{
    // 1. 创建 TCP socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "socket() failed\n";
        return;
    }

    // 2. 设置地址复用（避免重启时端口占用）
    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 设置为非阻塞（为 epoll 做准备）
    if (!set_nonblocking(listen_fd_)) {
        std::cerr << "set_nonblocking() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    // 4. 准备 IPv4 地址结构
    sockaddr_in address {};
    address.sin_family = AF_INET;   // IPv4
    address.sin_addr.s_addr = INADDR_ANY;    // 监听所有网卡
    address.sin_port = htons(static_cast<uint16_t>(config_.port)); // 端口号转网络字节序（大端）

    // 5. 绑定地址
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "bind() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    // 6. 开始监听
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        std::cerr << "listen() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    logger_.log(LogLevel::Info, "Listening on port " + std::to_string(config_.port));
}

// ---- epoll 实例 ----

void LinuxHttpServer::setup_epoll()
{
    // 1. 创建 epoll 实例
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::cerr << "epoll_create1() failed\n";
        return;
    }

    // 2. 准备事件结构：关注监听 fd 的可读事件
    epoll_event event {};
    event.events = EPOLLIN | EPOLLET; // 可读 + 边缘触发
    event.data.fd = listen_fd_; // 事件发生时返回这个 fd

    // 3. 把监听 fd 注册到 epoll
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::cerr << "epoll_ctl() failed\n";
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    logger_.log(LogLevel::Info, "epoll ready, listening fd " + std::to_string(listen_fd_));
}

// ---- 启动服务器 ----

bool LinuxHttpServer::start()
{
    logger_.start();  // 先启动日志器

    setup_listener();
    if (listen_fd_ < 0) {
        std::cerr << "setup_listener() failed\n";
        return false;
    }

    setup_epoll();
    if (epoll_fd_ < 0) {
        std::cerr << "setup_epoll() failed\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    logger_.log(LogLevel::Info, "Server started on port " + std::to_string(config_.port));

    // ----事件循环----
    std::vector<epoll_event> events(config_.max_events);
    running_ = true;

    while (running_) {
        // 等待事件，超时 100 毫秒
        const int count = ::epoll_wait(
            epoll_fd_, events.data(),
            static_cast<int>(events.size()), 100);

        // 遍历本轮触发的事件
        for (int i = 0; i < count; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t flags = events[i].events;

            // 监听 fd ，新连接到达
            if (fd == listen_fd_) {
                accept_clients();
                continue;
            }

            // 客户端 fd ，可读
            if ((flags & EPOLLIN) != 0) {
                handle_read(fd);
            }

            // 客户端 fd ，可写
            if ((flags & EPOLLOUT) != 0) {
                handle_write(fd);
            }
        }

        // 每轮循环末尾清理超时空闲连接
        sweep_idle_connections();
    }

    stop();  // 事件循环退出后有序释放所有资源
    return true;
}

// ---- 停止服务器 ----

void LinuxHttpServer::stop()
{
    // 1.通知事件循环退出
    running_ = false;

    // 2.停止线程池(等待所有工作线程结束)
    thread_pool_.stop(); 

    // 3.关闭所有客户端连接
    for (auto& [fd, conn] : connections_) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
    connections_.clear();

    // 4.关闭监听 socket
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    
    // 5.关闭 epoll 实例
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    // 6.停止日志线程（最后停止，之前的操作还可以记日志）
    logger_.stop(); 
}

// ---- 接受新连接 ----

void LinuxHttpServer::accept_clients()
{
    while (true) {
        // 1. 准备客户端地址结构
        sockaddr_in client_addr {};
        socklen_t len = sizeof(client_addr);

        // 2. 接受新连接（一步到位：非阻塞 + close-on-exec）
        const int client_fd = ::accept4(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);

        // 3. 判断结果
        if (client_fd < 0) {
            // ET 模式下循环到没有新连接为止
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 正常：当前没有更多连接了
            }
            // 真正的错误
            std::cerr << "accept4() failed, errno=" << errno << "\n";
            break;
        }

        // 4. 把新 client fd 注册到 epoll
        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // 可读 | 对端关闭 | 边缘触发
        event.data.fd = client_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);

        // 5. 加入连接表
        connections_.emplace(client_fd, Connection{
            .fd {client_fd},                // fd
            .in {},                         // in（空）
            .out {},                        // out (空)
            .close_after_write {true},      // close_after_write（未知，待解析后决定）
            .processing {false},            // processing
            .last_active_ms {now_ms()}      // last_active_ms
        });

        logger_.log(LogLevel::Info, "新连接 fd=" + std::to_string(client_fd)
                    + " 剩余连接数=" + std::to_string(connections_.size()));
    }
}

// ---- 关闭连接 ----

void LinuxHttpServer::close_connection(int fd)
{
    // 从 epoll 移除
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    // 关闭 fd
    ::close(fd);
    // 从连接表删除
    connections_.erase(fd);
    logger_.log(LogLevel::Info, "连接关闭 fd=" + std::to_string(fd)
                + " 剩余连接数=" + std::to_string(connections_.size()));
}

// ---- 处理请求并生成响应 ----

void LinuxHttpServer::process_request(int fd, const std::string& raw)
{
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = it->second;

    // ===== 第1步：解析 raw 请求字符串 =====
    const ParseResult parsed = parser_.parse(raw);
    if (parsed.status != ParseStatus::Complete) {
        // raw 应该是一个完整请求，如果不是则关闭连接
        close_connection(fd);
        return;
    }
    const HttpRequest& request = parsed.request;

    // 根据请求的 Connection 头决定是否 Keep-Alive
    if (!request.keep_alive()) {
        conn.close_after_write = true;
    } else {
        conn.close_after_write = false;
    }

    logger_.log(LogLevel::Info, "请求 fd=" + std::to_string(fd)
                + " " + request.method + " " + request.target
                + " " + request.version);

    // ===== 第2步：检查 method =====
    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse response(405, "Method Not Allowed");
        response.set_body("Method Not Allowed");
        response.set_header("Allow", "GET, HEAD");
        response.set_keep_alive(!conn.close_after_write);
        conn.out = response.serialize();
        switch_to_epollout(fd);
        return;
    }

    // ===== 第3步：target 映射到文件路径 =====
    std::string path = map_target_to_path(request.target);
    if (path.empty()) {
        HttpResponse response(403, "Forbidden");
        response.set_body("Forbidden");
        response.set_keep_alive(!conn.close_after_write);
        conn.out = response.serialize();
        switch_to_epollout(fd);
        return;
    }

    // ===== 第4步：打开文件 =====
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        HttpResponse response(404, "Not Found");
        response.set_body("Not Found");
        response.set_keep_alive(!conn.close_after_write);
        conn.out = response.serialize();
        switch_to_epollout(fd);
        return;
    }

    // ===== 第5步：读取文件内容 =====
    std::ostringstream contents;
    contents << file.rdbuf();
    file.close();

    // ===== 第6-7步：设置 MIME + 生成响应字符串 =====
    HttpResponse response(200, "OK");
    response.set_body(
        request.method == "HEAD" ? std::string{} : contents.str(),
        mime_type_for_path(path));
    response.set_keep_alive(!conn.close_after_write);
    conn.out = response.serialize();
    
    // 切换到写模式，等待 EPOLLOUT 发送响应
    switch_to_epollout(fd);
}

// ---- 路径穿越检测 ----

static bool contains_path_traversal(const std::string& path)
{
    // 拒绝包含 ".." 的路径（可向上穿越到站点根目录外）
    if (path.find("..") != std::string::npos) {
        return true;
    }
    // 拒绝反斜杠（windows 风格路径）
    if (path.find('\\') != std::string::npos) {
        return true;
    }
    return false;
}

// ---- 路径映射 ----

std::string LinuxHttpServer::map_target_to_path(const std::string& target)
{
    // 1. 去掉查询参数：/hello.txt?x=1 -> /hello.txt
    std::string clean = target;
    const std::size_t pos = clean.find('?');
    if (pos != std::string::npos) {
        clean = clean.substr(0, pos);
    }

    // 2. 根路径 -> 默认首页
    if (clean.empty() || clean == "/") {
        clean = "/index.html";
    }

    // 3. 路径穿越检查
    if (contains_path_traversal(clean)) {
        return {};  // 返回空串，表示路径非法
    }

    // 4. 去掉开头的 /
    while (!clean.empty() && clean.front() == '/') {
        clean.erase(clean.begin());
    }

    // 4. 拼接：config_.document_root + "/" + clean（如 www/index.html）
    return config_.document_root + "/" + clean;
}

// ---- 切换 epoll 事件：读 → 写 ----

void LinuxHttpServer::switch_to_epollout(int fd)
{
    epoll_event event{};
    event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET; // 关注可写 + 对端关闭 + 边缘触发
    event.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        std::cerr << "epoll_ctl MOD fd=" << fd << " failed, errno=" << errno << "\n";
        close_connection(fd);
    }
}

// ---- 切换 epoll 事件：写 → 读 ----

void LinuxHttpServer::switch_to_epollin(int fd)
{
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = it->second;

    // 重置处理状态，准备接收下一个 HTTP 请求
    conn.processing = false;

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // 关注可读 + 对端关闭 + 边缘触发
    event.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        std::cerr << "epoll_ctl MOD (->EPOLLIN) fd=" << fd << " failed\n";
        close_connection(fd);
    }
}

// ---- 读取客户端请求 ----

void LinuxHttpServer::handle_read(int fd)
{
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);
    if (it == connections_.end()) {
        return;  // fd 不在连接表中，防御性检查
    }
    Connection& conn = it->second;

    char buffer[4096];  // 临时接收缓冲区

    // ===== 外层循环：recv 直到内核缓冲区空 =====
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            // 读到了数据，追加到输入缓冲区
            conn.in.append(buffer, static_cast<std::size_t>(n));
            conn.last_active_ms = now_ms(); // 更新活跃时间

            // 尝试解析
            // ===== 内层循环：parse 直到 conn.in 耗尽或 Incomplete =====
            while (true) {
                const ParseResult parsed = parser_.parse(conn.in);
                
                if (parsed.status == ParseStatus::Complete) {
                    // 请求完整：取出一个完整请求的 raw 字符串，
                    std::string raw = conn.in.substr(0, parsed.consumed);
                    // 从输入缓冲区删除已处理部分
                    conn.in.erase(0, parsed.consumed);
                    // 标记正在处理，防止重复提交
                    conn.processing = true;
                    
                    // 提交到线程池（不阻塞事件循环！）
                    thread_pool_.submit([this, fd, raw = std::move(raw)] {
                        process_request(fd, raw);
                    });

                    // ← 继续内层循环，检查擦除后剩余数据是否也完整
                }
                else if (parsed.status == ParseStatus::BadRequest) {
                    std::cerr << "[解析] fd=" << fd << " 坏请求："
                              << parsed.error << "\n";
                    close_connection(fd);
                    return;
                }
                else {
                    // Incomplete → 退出内层 parse 循环，回到外层继续 recv
                    break;
                }
            }

        } else if (n == 0) {
            // 对端关闭连接(FIN)
            std::cout << "[读] fd=" << fd << " 对端关闭连接\n";
            close_connection(fd);
            return;

        } else {   // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核缓冲区暂时空了，退出读循环
                break;
            }
            // 真正的错误
            logger_.log(LogLevel::Error, "recv() fd=" + std::to_string(fd)
                        + " 错误 error=" + std::to_string(errno));
            close_connection(fd);
            return;
        }
    }
}


// ---- 写回响应 ----

void LinuxHttpServer::handle_write(int fd)
{
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = it->second;

    // 发送 conn.out 中剩余的全部数据
    const std::string& chunk = conn.out;
    const ssize_t n = ::send(fd, chunk.data(), chunk.size(), MSG_NOSIGNAL);

    if (n > 0) {
        // 1. 成功发送了 n 字节 -> 从发送缓冲区删除已发送部分
        conn.out.erase(0, static_cast<std::size_t>(n));

        // 2. 全部发完了？
        if (conn.out.empty()) {
            if (conn.close_after_write) {
                // 3. Connection: close -> 关闭连接
                close_connection(fd);
            } else {
                // 4. Connection: keep-alive -> 切回读模式，等待下一个请求
                switch_to_epollin(fd);
            }
        }
        // out 不为空 -> 剩余的等下次 EPOLLOUT 继续发

    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // 5. 内核发送缓冲区满了，等下次 EPOLLOUT
        return;

    } else {
        // 6. 对端关闭（收到 RST）或其他错误
        std::cerr << "send() fd=" << fd << " 错误 errno=" << errno << "\n";
        close_connection(fd);
    }
}

// ---- 清理超时连接

void LinuxHttpServer::sweep_idle_connections()
{
    const long long now = now_ms();
    const long long timeout_ms = static_cast<long long>(config_.keep_alive_seconds) * 1000;

    // 先收集待关闭的 fd（不能在遍历 map 的同时删除元素）
    std::vector<int> to_close;
    for (const auto& [fd, conn] : connections_) {
        if (conn.processing) {
            continue;  // 正在处理中，不关闭
        }
        if (now - conn.last_active_ms > timeout_ms) {
            to_close.push_back(fd);
        }
    }

    // 统一关闭
    for (int fd : to_close) {
        logger_.log(LogLevel::Info,
                    "超时关闭 fd=" + std::to_string(fd)
                    + " 空闲=" + std::to_string(now - connections_[fd].last_active_ms) + "ms");
        close_connection(fd);
    }
}

} // namespace tiny_http