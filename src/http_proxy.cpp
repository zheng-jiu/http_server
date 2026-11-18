#include "tiny_http/linux_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tiny_http {
namespace {

long long proxy_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string trim_ows(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }

    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string lower_header(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(), 
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return text;
}

// 找出当前一跳使用的头字段，转发时移除
std::unordered_set<std::string> hop_headers(const std::string& connection)
{
    std::unordered_set<std::string> names {
        "connection", "keep-alive", "proxy-connection",
        "te", "trailer", "transfer-encoding", "upgrade",
        "proxy-authorization", "proxy-authenticate"
    };

    std::istringstream stream(connection);
    std::string token;

    while (std::getline(stream, token, ',')) {
        names.insert(lower_header(trim_ows(token)));
    }

    return names;
}

bool valid_field(const std::string& name, const std::string& value)
{
    const std::string punctuation = "!#$%&'*+-.^_`|~";

    if (name.empty()) {
        return false;
    }

    for (unsigned char c : name) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z')) &&
            punctuation.find(static_cast<char>(c)) == std::string::npos) {
            return false;
        }
    }

    for (unsigned char c : value) {
        if ((c < 32 && c != '\t') || c == 127) {
            return false;
        }
    }

    return true;
}

// 空闲 Socket 有待读数据、EOF 或错误时，丢弃
bool idle_socket_is_usable(int fd)
{
    char byte;

    while (true) {
        const ssize_t n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

        if (n >= 0) {
            return false;
        }

        if (errno != EINTR) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }
}

} // namespace

int LinuxHttpServer::take_idle_upstream(BackendPool& pool)
{
    const long long now = proxy_now_ms();

    while (!pool.idle.empty()) {
        const IdleUpstream idle = pool.idle.back();
        pool.idle.pop_back();

        if (now - idle.idle_since_ms < backend_idle_timeout_ms_ &&
            idle_socket_is_usable(idle.fd)) {
            return idle.fd;
        }

        ::close(idle.fd);
    }

    return -1;
}

void LinuxHttpServer::discard_idle_upstreams(BackendPool& pool)
{
    for (const IdleUpstream& idle : pool.idle) {
        ::close(idle.fd);
    }

    pool.idle.clear();
}

void LinuxHttpServer::sweep_idle_upstreams(long long now)
{
    for (auto& [url, pool] : backend_pools_) {
        std::erase_if(pool.idle, [now](const IdleUpstream& idle) {
            if (now - idle.idle_since_ms < backend_idle_timeout_ms_) {
                return false;
            }

            ::close(idle.fd);
            return true;
        });
    }
}

const std::string* LinuxHttpServer::select_backend(const LocationRule& location)
{
    const std::vector<std::string>& backends = location.proxy_pass;

    if (backends.empty()) {
        return nullptr;
    }

    std::size_t& next = next_backend_[location.path];
    next %= backends.size();

    const long long now = proxy_now_ms();

    // 最多检查一轮，跳过冷却中或连接已用满的后端
    for (std::size_t checked = 0; checked < backends.size(); ++checked) {
        const std::size_t index = next;
        next = (next + 1) % backends.size();

        const std::string& url = backends[index];
        const auto it = backend_retry_at_.find(url);

        if (it != backend_retry_at_.end() && now < it->second) {
            continue;
        }

        const auto pool = backend_pools_.find(url);

        if (pool != backend_pools_.end() &&
            pool->second.busy >= backend_connection_limit_) {
            continue;
        }

        return &url;
    }

    return nullptr;
}

void LinuxHttpServer::mark_backend_failed(const std::string& url)
{
    backend_retry_at_[url] = proxy_now_ms() + backend_cooldown_ms_;

    const auto pool = backend_pools_.find(url);

    if (pool != backend_pools_.end()) {
        discard_idle_upstreams(pool->second);
    }

    logger_.log(
        LogLevel::Warn,
        "后端进入冷却 " + url + "，持续 "
        + std::to_string(backend_cooldown_ms_) + " ms"
    );
}

void LinuxHttpServer::start_proxy(
    int client_fd,
    const HttpRequest& request,
    const LocationRule& location)
{
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) {
        return;
    }

    Connection& conn = it->second;
    conn.processing = true;
    conn.rejecting = true; // 等待本次代理响应，暂停读取客户端
    conn.close_after_write =
        request.version != "HTTP/1.1" || !request.keep_alive();

    if (request.method != "GET" && request.method != "HEAD") {
        fail_proxy(client_fd, 405, "Method Not Allowed");
        return;
    }

    const std::string* selected = select_backend(location);

    if (selected == nullptr) {
        fail_proxy(client_fd, 503, "Service Unavailable");
        return;
    }

    const std::string& url = *selected;

    if (!url.starts_with("http://")) {
        fail_proxy(client_fd, 502, "Bad Gateway");
        return;
    }

    // 本版配置格式：http://IPv4:端口
    const std::string authority = url.substr(7);
    const auto colon = authority.rfind(':');

    if (colon == std::string::npos) {
        fail_proxy(client_fd, 502, "Bad Gateway");
        return;
    }

    const std::string host = authority.substr(0, colon);
    const std::string port_text = authority.substr(colon + 1);

    unsigned int port = 0;
    const auto [ptr, ec] = std::from_chars(
        port_text.data(),
        port_text.data() + port_text.size(),
        port
    );

    sockaddr_in address {};
    address.sin_family = AF_INET;

    if (ec != std::errc{} ||
        ptr != port_text.data() + port_text.size() ||
        port == 0 || port > 65535 ||
        ::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        fail_proxy(client_fd, 502, "Bad Gateway");
        return;
    }

    address.sin_port = htons(static_cast<std::uint16_t>(port));

    Upstream upstream;
    upstream.client_fd = client_fd;
    upstream.client_id = conn.id;
    upstream.backend_url = url;
    upstream.head_request = request.method == "HEAD";
    upstream.started_ms = proxy_now_ms();

    upstream.out =
        request.method + " " + request.target + " HTTP/1.1\r\n";

    const auto omitted = hop_headers(request.header("Connection"));

    for (const auto& [name, value] : request.headers) {
        if (!valid_field(name, value)) {
            fail_proxy(client_fd, 400, "Bad Request");
            return;
        }

        if (name == "host" || name == "content-length" ||
            name == "expect" || omitted.contains(name)) {
            continue;
        }

        upstream.out += name + ": " + value + "\r\n";
    }

    upstream.out += "Host: " + authority + "\r\n";

    if (request.headers.contains("content-length") ||
        !request.body.empty()) {
        upstream.out += "Content-Length: "
                      + std::to_string(request.body.size()) + "\r\n";
    }

    upstream.out += "\r\n";
    upstream.out += request.body;

    BackendPool& pool = backend_pools_[url];
    int fd = take_idle_upstream(pool);

    if (fd >= 0) {
        upstream.connecting = false;
        upstream.reused = true;
    } else {
        fd = ::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0
        );

        if (fd < 0) {
            fail_proxy(client_fd, 502, "Bad Gateway");
            return;
        }

        const int result = ::connect(
            fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        );

        if (result < 0 && errno != EINPROGRESS) {
            ::close(fd);
            mark_backend_failed(url);
            fail_proxy(client_fd, 502, "Bad Gateway");
            return;
        }

        upstream.connecting = result < 0;
    }

    upstream.fd = fd;
    upstreams_.emplace(fd, std::move(upstream));
    ++pool.busy;
    conn.upstream_fd = fd;

    // 当前客户端请求已经收全，等待后端响应
    epoll_event client_event {};
    client_event.events = EPOLLRDHUP | EPOLLET;
    client_event.data.fd = client_fd;

    if (::epoll_ctl(
            epoll_fd_,
            EPOLL_CTL_MOD,
            client_fd,
            &client_event) < 0) {
        close_connection(client_fd);
        return;
    }

    epoll_event event {};
    event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.fd = fd;

    if (::epoll_ctl(
            epoll_fd_,
            EPOLL_CTL_ADD,
            fd,
            &event) < 0) {
        fail_proxy(client_fd, 502, "Bad Gateway");
    }
}

bool LinuxHttpServer::parse_upstream_headers(Upstream& upstream)
{
    std::istringstream stream(
        upstream.in.substr(0, upstream.header_size - 4)
    );

    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream status_line(line);
    std::string version, code, reason;

    if (!(status_line >> version >> code)) {
        return false;
    }

    std::getline(status_line, reason);

    const auto [ptr, ec] = std::from_chars(
        code.data(),
        code.data() + code.size(),
        upstream.status
    );

    if ((version != "HTTP/1.0" && version != "HTTP/1.1") ||
        code.size() != 3 ||
        ec != std::errc{} ||
        ptr != code.data() + code.size() ||
        upstream.status < 200 || upstream.status > 599 ||
        !valid_field("reason", reason)) {
        return false;
    }

    std::vector<std::pair<std::string, std::string>> headers;
    std::string connection;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto colon = line.find(':');

        if (colon == std::string::npos) {
            return false;
        }

        std::string name = lower_header(line.substr(0, colon));
        std::string value = trim_ows(line.substr(colon + 1));

        if (!valid_field(name, value)) {
            return false;
        }

        if (name == "transfer-encoding") {
            return false;
        }

        if (name == "content-length") {
            if (upstream.content_length.has_value()) {
                return false;
            }

            std::size_t length = 0;

            const auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                length
            );

            if (error != std::errc{} ||
                end != value.data() + value.size()) {
                return false;
            }

            upstream.content_length = length;
        }

        if (name == "connection") {
            connection += value + ",";
        }

        headers.emplace_back(std::move(name), std::move(value));
    }

    upstream.no_body =
        upstream.head_request ||
        upstream.status == 204 ||
        upstream.status == 304;

    if (!upstream.no_body &&
        upstream.content_length.has_value() &&
        *upstream.content_length > proxy_body_limit_) {
        return false;
    }

    upstream.response_head =
        "HTTP/1.1 " + code + " " + trim_ows(reason) + "\r\n";

    const auto omitted = hop_headers(connection);
    upstream.keep_alive = version == "HTTP/1.1" && !omitted.contains("close");

    for (const auto& [name, value] : headers) {
        if (name == "content-length" || omitted.contains(name)) {
            continue;
        }

        upstream.response_head += name + ": " + value + "\r\n";
    }

    return true;
}

void LinuxHttpServer::handle_upstream(
    int fd,
    std::uint32_t flags)
{
    auto it = upstreams_.find(fd);

    if (it == upstreams_.end()) {
        return;
    }

    Upstream& upstream = it->second;
    const int client_fd = upstream.client_fd;

    auto client = connections_.find(client_fd);

    if (client == connections_.end() ||
        client->second.id != upstream.client_id) {
        close_upstream(fd);
        return;
    }

    // 第一阶段：确认非阻塞连接的结果
    if (upstream.connecting) {
        if ((flags & (EPOLLOUT | EPOLLERR | EPOLLHUP)) == 0) {
            return;
        }

        int error = 0;
        socklen_t length = sizeof(error);

        if (::getsockopt(
                fd,
                SOL_SOCKET,
                SO_ERROR,
                &error,
                &length) < 0) {
            fail_proxy(client_fd, 502, "Bad Gateway");
            return;
        }

        if (error != 0) {
            fail_proxy(client_fd, 502, "Bad Gateway", true);
            return;
        }

        upstream.connecting = false;
    }

    // 第二阶段：发送请求，遇到 EAGAIN 保留发送位置
    while (upstream.sent < upstream.out.size()) {
        const ssize_t n = ::send(
            fd,
            upstream.out.data() + upstream.sent,
            upstream.out.size() - upstream.sent,
            MSG_NOSIGNAL
        );

        if (n > 0) {
            upstream.sent += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        const bool backend_failure = !upstream.reused || upstream.sent != 0;
        fail_proxy(client_fd, 502, "Bad Gateway", backend_failure);
        return;
    }

    if (!upstream.out.empty()) {
        upstream.out.clear();
        upstream.sent = 0;

        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        event.data.fd = fd;

        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_MOD,
                fd,
                &event) < 0) {
            fail_proxy(client_fd, 502, "Bad Gateway");
            return;
        }
    }

    // 第三阶段：接收响应
    char buffer[8192];

    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            upstream.in.append(
                buffer,
                static_cast<std::size_t>(n)
            );

            if (upstream.in.size() >
                proxy_header_limit_ + proxy_body_limit_) {
                fail_proxy(client_fd, 502, "Bad Gateway");
                return;
            }

            if (upstream.header_size == 0) {
                const auto end = upstream.in.find("\r\n\r\n");

                if (end == std::string::npos) {
                    if (upstream.in.size() > proxy_header_limit_) {
                        fail_proxy(client_fd, 502, "Bad Gateway");
                        return;
                    }

                    continue;
                }

                upstream.header_size = end + 4;

                if (upstream.header_size > proxy_header_limit_ ||
                    !parse_upstream_headers(upstream)) {
                    fail_proxy(client_fd, 502, "Bad Gateway");
                    return;
                }
            }

            const std::size_t received =
                upstream.in.size() - upstream.header_size;

            if ((upstream.no_body && received != 0) ||
                (!upstream.no_body && upstream.content_length.has_value() &&
                 received > *upstream.content_length)) {
                fail_proxy(client_fd, 502, "Bad Gateway");
                return;
            }

            if (upstream.no_body ||
                (upstream.content_length.has_value() &&
                 received == *upstream.content_length)) {
                finish_proxy(fd);
                return;
            }

            if (received > proxy_body_limit_) {
                fail_proxy(client_fd, 502, "Bad Gateway");
                return;
            }

            continue;
        }

        if (n == 0) {
            // 无长度字段时，以后端正常关闭连接作为正文结束
            if (upstream.header_size > 0 &&
                !upstream.content_length.has_value()) {
                finish_proxy(fd);
            } else {
                const bool backend_failure =
                    !upstream.reused || !upstream.in.empty();
                fail_proxy(client_fd, 502, "Bad Gateway", backend_failure);
            }

            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        const bool backend_failure = !upstream.reused || !upstream.in.empty();
        fail_proxy(client_fd, 502, "Bad Gateway", backend_failure);
        return;
    }
}

void LinuxHttpServer::finish_proxy(int upstream_fd)
{
    auto it = upstreams_.find(upstream_fd);

    if (it == upstreams_.end()) {
        return;
    }

    Upstream& upstream = it->second;
    const int client_fd = upstream.client_fd;

    auto client = connections_.find(client_fd);

    if (client == connections_.end() ||
        client->second.id != upstream.client_id) {
        close_upstream(upstream_fd);
        return;
    }

    std::string response = upstream.response_head;
    std::size_t body_size = 0;

    if (upstream.no_body) {
        // HEAD、304 的长度可以描述对应资源；204 省略长度字段
        if (upstream.status != 204 &&
            upstream.content_length.has_value()) {
            response += "Content-Length: "
                      + std::to_string(*upstream.content_length)
                      + "\r\n";
        }
    } else {
        body_size = upstream.content_length.value_or(
            upstream.in.size() - upstream.header_size
        );

        response += "Content-Length: "
                  + std::to_string(body_size)
                  + "\r\n";
    }

    response += client->second.close_after_write
        ? "Connection: close\r\n\r\n"
        : "Connection: keep-alive\r\n\r\n";

    response.append(
        upstream.in,
        upstream.header_size,
        body_size
    );

    client->second.out = std::move(response);

    const bool reusable = upstream.keep_alive &&
        (upstream.no_body || upstream.content_length.has_value());

    release_upstream(upstream_fd, reusable);
    switch_to_epollout(client_fd);
}

void LinuxHttpServer::fail_proxy(
    int client_fd,
    int status,
    const std::string& reason,
    bool backend_failure)
{
    auto it = connections_.find(client_fd);

    if (it == connections_.end()) {
        return;
    }

    // 先记录故障节点，再释放后端连接
    if (backend_failure) {
        const auto upstream = upstreams_.find(it->second.upstream_fd);

        if (upstream != upstreams_.end() &&
            upstream->second.client_id == it->second.id) {
            mark_backend_failed(upstream->second.backend_url);
        }
    }

    close_upstream(it->second.upstream_fd);

    HttpResponse response(status, reason);
    response.set_body("");
    response.set_keep_alive(!it->second.close_after_write);

    if (status == 405) {
        response.set_header("Allow", "GET, HEAD");
    }

    it->second.out = response.serialize();
    it->second.processing = true;
    it->second.rejecting = true;

    logger_.log(
        LogLevel::Warn,
        "代理请求失败 fd=" + std::to_string(client_fd)
        + " " + reason
    );

    switch_to_epollout(client_fd);
}

void LinuxHttpServer::release_upstream(int fd, bool reusable)
{
    auto it = upstreams_.find(fd);

    if (it == upstreams_.end()) {
        return;
    }

    const Upstream& upstream = it->second;
    auto client = connections_.find(upstream.client_fd);

    if (client != connections_.end() &&
        client->second.id == upstream.client_id &&
        client->second.upstream_fd == fd) {
        client->second.upstream_fd = -1;
    }

    BackendPool& pool = backend_pools_.at(upstream.backend_url);
    --pool.busy;

    const bool detached =
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;

    const long long now = proxy_now_ms();
    const auto retry = backend_retry_at_.find(upstream.backend_url);
    const bool cooling =
        retry != backend_retry_at_.end() && now < retry->second;

    if (reusable && detached && !cooling && idle_socket_is_usable(fd)) {
        pool.idle.push_back({fd, now});
    } else {
        ::close(fd);
    }

    upstreams_.erase(it);
}

void LinuxHttpServer::close_upstream(int fd)
{
    release_upstream(fd, false);
}

} // namespace tiny_http
