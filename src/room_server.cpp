#include "tiny_room/protocol.h"

#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace tiny_room {

class RoomServer {
public:
    RoomServer() = default;
    RoomServer(const RoomServer&) = delete;
    RoomServer& operator=(const RoomServer&) = delete;

    ~RoomServer() 
    {
        for (const auto& [fd, conn] : connections_) {
            ::close(fd);
        }
        if (signal_fd_ >= 0) ::close(signal_fd_);
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    void run(const sigset_t& stop_signals)
    {
        listen_fd_ = checked(::socket(
            AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0),
            "socket");
        
        int reuse = 1;
        checked(::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                            &reuse, sizeof(reuse)), "setsockopt");

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(9090);

        checked(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)), "bind");
        checked(::listen(listen_fd_, SOMAXCONN), "listen");

        epoll_fd_ = checked(::epoll_create1(EPOLL_CLOEXEC), "epoll_create1");
        signal_fd_ = checked(::signalfd(-1, &stop_signals,
            SFD_NONBLOCK | SFD_CLOEXEC), "signalfd");

        checked(watch(EPOLL_CTL_ADD, listen_fd_, EPOLLIN | EPOLLET),
                "epoll add listener");
        checked(watch(EPOLL_CTL_ADD, signal_fd_, EPOLLIN),
                "epoll add signalfd");
        
        std::cout << "Room server: 127.0.0.1:9090\n" << std::flush;
        epoll_event events[64] {};

        while (true) {
            const int count = ::epoll_wait(epoll_fd_, events, 64, -1);
            if (count < 0 && errno == EINTR) continue;
            checked(count, "epoll_wait");

            for (int i = 0; i < count; ++i) {
                const int fd = events[i].data.fd;
                const std::uint32_t flags = events[i].events;

                if (fd == signal_fd_) {
                    signalfd_siginfo info {};
                    ssize_t n;
                    do {
                        n = ::read(signal_fd_, &info, sizeof(info));
                    } while (n < 0 && errno == EINTR);
                    if (n < 0 && errno == EAGAIN) continue;
                    if (n != static_cast<ssize_t>(sizeof(info))) {
                        throw std::runtime_error("read(signalfd) failed");
                    }
                    std::cout << "收到退出信号，服务器停止\n";
                    return;
                }

                if (fd == listen_fd_) {
                    accept_clients();
                    continue;
                }

                if (connections_.find(fd) == connections_.end()) continue;
                if (flags & (EPOLLERR | EPOLLHUP)) {
                    close_connection(fd);
                    continue;
                }
                if ((flags & (EPOLLIN | EPOLLRDHUP)) && !handle_read(fd)) {
                    continue;
                }

                // 收到请求后可立即尝试非阻塞发送；发不动再等 EPOLLOUT
                handle_write(fd);
            }
        }
    }

private:
    struct Connection {
        std::string in;
        std::string out;

        // 空字符串表示尚未加入房间
        std::string room;

        bool read_closed {false};
    };

private:
    static int checked(int result, const char* operation)
    {
        if (result < 0) {
            throw std::system_error(errno, std::generic_category(), operation);
        }
        return result;
    }

    int watch(int operation, int fd, std::uint32_t flags)
    {
        epoll_event event {};
        event.events = flags;
        event.data.fd = fd;
        return ::epoll_ctl(epoll_fd_, operation, fd, &event);
    }

    void close_connection(int fd)
    {
        if (connections_.erase(fd) == 0) return;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }

    //广播给同房间成员（包含发送者）
    // 返回 false 表示发送者已关闭，调用者不能继续使用它的 Connection
    bool broadcast_room(int sender_fd, const std::string& body)
    {
        std::unordered_map<int, Connection>::iterator sender = connections_.find(sender_fd);
        if (sender == connections_.end()) {
            return false;
        }

        const std::string room = sender->second.room;
        const std::string packet = encode_message({MessageType::RoomBroadcast, body});

        std::vector<int> to_close;

        for (auto& [fd, conn] : connections_) {
            // 不同房间或已经半关闭的连接，不再添加新广播
            if (conn.room != room || conn.read_closed) {
                continue;
            }

            // 保留原来的 1 MiB 发送积压上限
            if (conn.out.size() + packet.size() > 1024 * 1024) {
                to_close.push_back(fd);
                continue;
            }

            // 追加广播，不能覆盖此前尚未发送的数据
            conn.out.append(packet);

            // 接收者可能没有发请求，也需要安排发送
            if (watch(
                    EPOLL_CTL_MOD,
                    fd,
                    EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLET) < 0) {
                to_close.push_back(fd);
            }       
        }

        // 遍历结束后再删除，避免破坏当前遍历
        for (int fd : to_close) {
            close_connection(fd);
        }

        // 发送者也可能因积压超限被关闭
        return connections_.find(sender_fd) != connections_.end();
    }

    void accept_clients()
    {
        while (true) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0 && errno == EINTR) continue;
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            checked(fd, "accept4");

            connections_.try_emplace(fd);
            if (watch(EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLRDHUP | EPOLLET) < 0) {
                close_connection(fd);
                continue;
            }
            std::cout << "新连接 fd=" << fd << '\n';
        }
    }

    // 返回 false 表示连接已关闭，调用者不能继续处理它
    bool handle_read(int fd)
    {
        Connection& conn = connections_.at(fd);
        char buffer[4096];

        while (true) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(n));
                while (true) {
                    Message message;
                    const DecodeStatus status = try_decode_message(conn.in, message);
                    if (status == DecodeStatus::Incomplete) break;
                    if (status == DecodeStatus::TooLarge) {
                        close_connection(fd);
                        return false;
                    }

                    Message response;

                    switch (message.type) {
                    case MessageType::Echo:
                        // 保留原来的回显功能
                        response = {MessageType::EchoReply, message.body};
                        break;

                    case MessageType::JoinRoom:
                        if (message.body.empty() || message.body.size() > 32) {
                            // 房间名无效：返回错误，不改变原来的房间
                            response = {
                                MessageType::Error,
                                "room name must be 1..32 bytes"
                            };
                        } 
                        else {
                            // 首次加入或切换房间
                            conn.room = message.body;
                            response = {MessageType::JoinRoomReply, conn.room};

                            std::cout << "fd=" << fd << " 加入房间：" << conn.room << '\n';
                        }
                        break;
                    
                    case MessageType::RoomMessage:
                        if (conn.room.empty()) {
                            response = {MessageType::Error, "join a room first"};
                            break;
                        }

                        if (!broadcast_room(fd, message.body)) {
                            return false;
                        }

                        // 广播已放入各连接的 out
                        // 继续解析下一条消息，不再生成普通回复
                        continue;

                    default:
                        // 暂不接受其他消息类型
                        close_connection(fd);
                        return false;
                    }

                    std::string reply = encode_message(response);

                    // 暂定每连接最多积压 1 MiB，避免慢客户端无限占用内存
                    if (conn.out.size() + reply.size() > 1024 * 1024) {
                        close_connection(fd);
                        return false;
                    }
                    
                    conn.out.append(reply);
                }
                continue;
            }
            if (n == 0) {
                conn.read_closed = true; // 对端不再发送，仍可先发完已有回复
                return true;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            close_connection(fd);
            return false;
        }
    }

    void handle_write(int fd)
    {
        Connection& conn = connections_.at(fd);
        while (!conn.out.empty()) {
            const ssize_t n = ::send(fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
            if (n > 0) {
                conn.out.erase(0, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close_connection(fd);
            return;
        }

        if (conn.read_closed && conn.out.empty()) {
            close_connection(fd);
            return;
        }

        // 未半关闭的连接始终关注读；只有积压回复时才关注写
        std::uint32_t flags = EPOLLET;
        if (!conn.read_closed) flags |= EPOLLIN | EPOLLRDHUP;
        if (!conn.out.empty()) flags |= EPOLLOUT;
        if (watch(EPOLL_CTL_MOD, fd, flags) < 0) close_connection(fd); 
    }

private:
    int listen_fd_ {-1};
    int epoll_fd_ {-1};
    int signal_fd_ {-1};
    std::unordered_map<int, Connection> connections_;
};

} // namespace tiny_room

int main() {
    sigset_t stop_signals {};
    ::sigemptyset(&stop_signals);
    ::sigaddset(&stop_signals, SIGINT);
    ::sigaddset(&stop_signals, SIGTERM);

    if (::sigprocmask(SIG_BLOCK, &stop_signals, nullptr) < 0) {
        std::cerr << "sigprocmask() failed, errno=" << errno << '\n';
        return 1;
    }

    try {
        tiny_room::RoomServer server;
        server.run(stop_signals);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}