#include "tiny_http/linux_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <cerrno>
#include <chrono>

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

    // 2. 注册监听 fd
    epoll_event listen_event {};
    listen_event.events = EPOLLIN | EPOLLET; // 可读 + 边缘触发
    listen_event.data.fd = listen_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listen_event) < 0) {
        std::cerr << "epoll_ctl() listen_fd failed\n";

        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // 3. 创建 eventfd
    completion_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (completion_fd_ < 0) {
        std::cerr << "eventfd() failed\n";

        ::close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // 4. 把 eventfd 注册进 epoll
    epoll_event completion_event {};
    completion_event.events = EPOLLIN;
    completion_event.data.fd = completion_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, completion_fd_, &completion_event) < 0) {
        std::cerr << "epoll_ctl() completion_fd failed\n";

        ::close(completion_fd_);
        completion_fd_ = -1;

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

    // 接收 main() 中已经屏蔽的退出信号
    sigset_t stop_signals {};
    ::sigemptyset(&stop_signals);
    ::sigaddset(&stop_signals, SIGINT);
    ::sigaddset(&stop_signals, SIGTERM);

    signal_fd_ = ::signalfd(
        -1,
        &stop_signals,
        SFD_NONBLOCK | SFD_CLOEXEC
    );

    if (signal_fd_ < 0) {
        std::cerr << "signalfd() failed, errno=" << errno << "\n";
        stop();
        return false;
    }

    // 将退出信号加入现有 epoll 事件循环
    epoll_event signal_event {};
    signal_event.events = EPOLLIN;
    signal_event.data.fd = signal_fd_;

    if (::epoll_ctl(
        epoll_fd_,
        EPOLL_CTL_ADD,
        signal_fd_,
        &signal_event) < 0)
    {
        std::cerr << "epoll_ctl() signal_fd failed, errno=" << errno << "\n";
        stop();
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

            // 退出信号：在正常的 I/O 线程执行流程中处理
            if (fd == signal_fd_) {
                struct signalfd_siginfo info {};
                ssize_t n;

                do {
                    n = ::read(signal_fd_, &info, sizeof(info));
                } while (n < 0 && errno == EINTR);

                if (n == static_cast<ssize_t>(sizeof(info))) {
                    std::cout << "收到退出信号 " << info.ssi_signo << "，准备停止服务器\n";

                    running_ = false;
                    break; // 先退出本轮 for
                }

                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }

                std::cerr << "read(signalfd) failed\n";
                stop();
                return false;
            }

            // 监听 fd ，新连接到达
            if (fd == listen_fd_) {
                accept_clients();
                continue;
            }

            // worker 完成通知 fd ，有响应结果
            if (fd == completion_fd_) {
                handle_completions();
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
        } // for 结束

        if (!running_) {
            break; // 再退出外层 while
        }

        // 每轮循环末尾清理超时空闲连接
        sweep_idle_connections();
    } // while 结束

    stop();  // 事件循环退出后有序释放所有资源
    return true;
}

// ---- 停止服务器 ----

void LinuxHttpServer::stop()
{
    // 通知事件循环退出
    running_ = false;

    // 停止线程池(等待所有工作线程结束)
    thread_pool_.stop();

    // worker 已全部结束，不会再向完成队列添加结果
    // 释放尚未移交给 Connection 的文件 fd
    while (std::optional<tiny_http::LinuxHttpServer::ResponseResult> result = completion_queue_.try_pop()) {
        if (result->file_fd >= 0) {
            ::close(result->file_fd);
        }
    }

    // 关闭所有客户端连接
    for (auto& [fd, conn] : connections_) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

        if (conn.file_fd >= 0) {
            ::close(conn.file_fd);
            conn.file_fd = -1;
        }

        ::close(fd);
    }

    connections_.clear();

    // 关闭监听 socket
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    
    // 关闭 worker 完成通知 eventfd
    if (completion_fd_ >= 0) {
        ::close(completion_fd_);
        completion_fd_ = -1;
    }

    // 关闭退出信号 fd
    if (signal_fd_ >= 0) {
        ::close(signal_fd_);
        signal_fd_ = -1;
    }

    // 关闭 epoll 实例
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    // 停止日志线程（最后停止，之前的操作还可以记日志）
    logger_.stop(); 
}

// ---- 接受新连接 ----

void LinuxHttpServer::accept_clients()
{
    while (true) {
        // 准备客户端地址结构
        sockaddr_in client_addr {};
        socklen_t len = sizeof(client_addr);

        // 接受新连接（一步到位：非阻塞 + close-on-exec）
        const int client_fd = ::accept4(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);

        // 判断结果
        if (client_fd < 0) {
            // ET 模式下循环到没有新连接为止
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 正常：当前没有更多连接了
            }
            // 真正的错误
            std::cerr << "accept4() failed, errno=" << errno << "\n";
            break;
        }

        int flag = 1;

        // 禁用 Nagle 算法，让小块数据不要因为等待前一个包 ACK 而被延迟
        if (::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            std::cerr << "setsockopt(TCP_NODELAY) failed, fd=" << client_fd << " errno=" << errno << "\n";
        }

        // 把新 client fd 注册到 epoll
        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // 可读 | 对端关闭 | 边缘触发
        event.data.fd = client_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);

        // 加入连接表
        const std::uint64_t connection_id = next_connection_id_++;
        connections_.emplace(client_fd, Connection{
            .fd {client_fd},                // fd
            .id {connection_id},            // id
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
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);

    if (it != connections_.end()) {
        if (it->second.file_fd >= 0) {
            ::close(it->second.file_fd);
            it->second.file_fd = -1;
        }
    }

    // 从 epoll 移除
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    // 关闭客户端 socket
    ::close(fd);

    // 删除连接状态
    connections_.erase(fd);

    logger_.log(LogLevel::Info, "连接关闭 fd=" + std::to_string(fd)
                + " 剩余连接数=" + std::to_string(connections_.size()));
}

// ---- 处理请求并生成响应 ----

LinuxHttpServer::ResponseResult LinuxHttpServer::process_request(int fd, std::uint64_t connection_id, const std::string& raw)
{
    ResponseResult result;

    result.fd = fd;
    result.connection_id = connection_id;

    // 1. 解析 HTTP 请求
    const ParseResult parsed = parser_.parse(raw);

    if (parsed.status != ParseStatus::Complete) {
        // worker 不再自己 close
        // 只告诉 I/O 线程：这个连接需要关闭
        result.close_connection = true;
        return result;
    }

    const HttpRequest& request = parsed.request;

    // 2. 是否 Keep-Alive
    result.close_after_write = !request.keep_alive();

    logger_.log(LogLevel::Info, "请求 fd=" + std::to_string(fd)
        + " " + request.method + " " + request.target + " " + request.version);

    // 3. method 检查
    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse response(405, "Method Not Allowed");

        response.set_body("Method Not Allowed");
        response.set_header("Allow", "GET, HEAD");
        response.set_keep_alive(!result.close_after_write);

        result.response = response.serialize();
        return result;
    }

    // 4. URL -> 文件路径
    const std::string path = map_target_to_path(request.target);

    if (path.empty()) {
        HttpResponse response(403, "Forbidden");

        response.set_body("Forbidden");
        response.set_keep_alive(!result.close_after_write);
    
        result.response = response.serialize();
        return result;
    }

    // 5. 打开静态文件
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);

    if (file_fd < 0) {
        HttpResponse response(404, "Not Found");

        response.set_body("Not Found");
        response.set_keep_alive(!result.close_after_write);

        result.response = response.serialize();
        return result;
    }

    // 6. 获取文件大小
    struct stat file_stat {};

    if (::fstat(file_fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
        ::close(file_fd);

        HttpResponse response(404, "Not Found");
        
        response.set_body("Not Found");
        response.set_keep_alive(!result.close_after_write);

        result.response = response.serialize();
        return result;
    }

    const std::size_t file_size = static_cast<std::size_t>(file_stat.st_size);

    // 7. 生成 HTTP 响应头
    HttpResponse response(200, "OK");

    // body 留空，文件正文后续由 I/O 线程通过 sendfile() 发送
    response.set_body(std::string{}, mime_type_for_path(path));

    // Content-Length 必须是真实文件大小
    response.set_header("Content-Length", std::to_string(file_size));

    response.set_keep_alive(!result.close_after_write);

    result.response = response.serialize();

    // HEAD 只返回响应头，不发送文件正文
    if (request.method == "HEAD") {
        ::close(file_fd);
        return result;
    }

    // GET: 把文件 fd 和大小交给 I/O 线程
    result.file_fd = file_fd;
    result.file_size = file_size;

    return result;
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

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // 关注可读 + 对端关闭 + 边缘触发
    event.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        std::cerr << "epoll_ctl MOD (->EPOLLIN) fd=" << fd << " failed\n";
        close_connection(fd);
    }
}

bool LinuxHttpServer::try_submit_next_request(int fd) {
    std::unordered_map<int, Connection>::iterator it = connections_.find(fd);
    if (it == connections_.end()) {
        return false;
    }

    Connection& conn = it->second;

    // 当前已经有请求正在处理中
    if (conn.processing) {
        return true;
    }

    const ParseResult parsed = parser_.parse(conn.in);

    if (parsed.status == ParseStatus::Incomplete) {
        return true;
    }

    if (parsed.status == ParseStatus::BadRequest) {
        close_connection(fd);
        return false;
    }

    // 取出一个完整请求
    std::string raw = conn.in.substr(0, parsed.consumed);

    conn.in.erase(0, parsed.consumed);

    // 从现在开始，该连接不允许再提交第二个请求
    conn.processing = true;

    const std::uint64_t connection_id = conn.id;

    thread_pool_.submit(
        [this, fd, connection_id, raw = std::move(raw)] {
            ResponseResult result = process_request(fd, connection_id, raw);

            completion_queue_.push(std::move(result));

            std::uint64_t one = 1;
            const ssize_t n = ::write(completion_fd_, &one, sizeof(one));

            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                logger_.log(
                    LogLevel::Error,
                    "write(eventfd) failed errno="
                    + std::to_string(errno)
                );
            }
        }
    );

    return true;
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

            // 尝试提交一个请求
            // 如果 processing == true，这里什么都不会提交
            if (!try_submit_next_request(fd)) {
                return;
            }

            // 注意：不能 return
            // 外层仍然继续 recv，直到 EAGAIN
        } 
        else if (n == 0) {
            // 对端关闭连接(FIN)
            std::cout << "[读] fd=" << fd << " 对端关闭连接\n";
            close_connection(fd);
            return;
        } 
        else {  // n < 0
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

    // 1. 先发送 HTTP 响应头
    // ET 模式：一直发送，直到全部发完或内核发送缓冲区满
    while (!conn.out.empty()) {
        const ssize_t n = ::send(fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);

        if (n > 0) {
            // 删除已经成功交给内核发送缓冲区的数据
            conn.out.erase(0, static_cast<std::size_t>(n));

            conn.last_active_ms = now_ms();
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 内核发送缓冲区暂时满了，当前不可写
            // 保留 conn.out 剩余数据，等待下一次 EPOLLOUT 后继续发送
            return;
        }

        // 对端关闭或其他错误
        std::cerr << "send() fd=" << fd << " 错误 errno=" << errno << "\n";

        close_connection(fd);
        return;
    }

    // 2. HTTP 头发送完成后，通过 sendfile 发送静态文件正文
    while (conn.file_fd >= 0 && conn.file_remaining > 0) {
        const ssize_t n = ::sendfile(fd, conn.file_fd, &conn.file_offset, conn.file_remaining);

        if (n > 0) {
            conn.file_remaining -= static_cast<std::size_t>(n);

            conn.last_active_ms = now_ms();
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 内核发送缓冲区暂时满了，当前不可写
            // 保留文件发送状态，等待下一次 EPOLLOUT 后继续发送
            return;
        }

        // n == 0 但文件仍未发完，或者发生其他错误
        std::cerr << "sendfile() fd=" << fd << " 错误 errno=" << errno << "\n";

        close_connection(fd);
        return;
    }

    // 3. 文件正文已经全部发送完成，释放文件 fd
    if (conn.file_fd >= 0) {
        ::close(conn.file_fd);
        conn.file_fd = -1;
        conn.file_offset = 0;
        conn.file_remaining = 0;
    }

    // 4. Connection: close
    if (conn.close_after_write) {
        close_connection(fd);
        return;
    }

    // 5. Keep-Alive：
    // 当前请求完整结束，允许处理同一连接的下一个请求
    conn.processing = false;

    switch_to_epollin(fd);

    // 下一个请求可能已经提前缓存到 conn.in
    if (!try_submit_next_request(fd)) {
        return;
    }
}

// ---- 处理 worker 完成通知 ----

void LinuxHttpServer::handle_completions()
{
    // 1. 消费 eventfd 通知（计数器清零）
    std::uint64_t count = 0;
    const ssize_t n = ::read(completion_fd_, &count, sizeof(count));

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "read(eventfd) failed, errno=" << errno << "\n";
        return;
    }

    // 2. 把当前完成队列中的结果全部取出来
    while (true) {
        std::optional<tiny_http::LinuxHttpServer::ResponseResult> result = completion_queue_.try_pop();

        if (!result.has_value()) {
            break; // 队列空了
        }

        // 3. fd 是否仍然存在？
        std::unordered_map<int, Connection>::iterator it = connections_.find(result->fd);

        if (it == connections_.end()) {
            // worker 已经打开了文件，但连接没了
            // 必须释放文件 fd，防止泄漏
            if (result->file_fd >= 0) {
                ::close(result->file_fd);
            }

            continue; // fd 已经关闭了，忽略
        }

        Connection& conn = it->second;

        // 4. fd 是否已经被复用成另一条连接？
        if (conn.id != result->connection_id) {
            if (result->file_fd >= 0) {
                ::close(result->file_fd);
            }

            continue; // fd 已经被复用，忽略
        }

        // 5. worker 要求直接关闭连接
        if (result->close_connection) {
            if (result->file_fd >= 0) {
                ::close(result->file_fd);
            }

            close_connection(result->fd);
            continue;
        }

        // 6. 接管 HTTP 响应头
        conn.out = std::move(result->response);
        conn.close_after_write = result->close_after_write;

        // 7. 接管静态文件发送状态
        conn.file_fd = result->file_fd;
        conn.file_offset = 0;
        conn.file_remaining = result->file_size;

        // 表示文件 fd 所有权已经交给 Connection
        result->file_fd = -1;

        // 8. 由 I/O 线程负责实际发送
        switch_to_epollout(result->fd);
    }
}

// ---- 清理超时连接 ----

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