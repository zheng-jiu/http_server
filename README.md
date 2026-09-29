# TinyHttpServer

一个基于 **C++20 + Linux** 的网络编程实践项目，包含 HTTP 静态资源服务器和 TCP 长连接房间服务。

项目围绕非阻塞 I/O、epoll 事件循环、协议解析、线程协作、连接生命周期、服务端主动推送及性能优化展开。

| 程序 | 用途 | 默认监听地址 | 线程模型 |
| --- | --- | --- | --- |
| `tiny_httpd` | HTTP 静态资源服务 | `0.0.0.0:8080` | I/O 线程 + 工作线程池 |
| `tiny_roomd` | TCP 长连接回显与房间广播 | `127.0.0.1:9090` | 单线程事件循环 |

两个程序独立运行，使用不同的应用层协议。房间服务不是 HTTP 接口，也不是完整的游戏服务器。

项目定位为持续迭代的学习项目，不作为可直接用于生产环境的成熟服务器。

## 技术栈

C++20 · Linux · TCP / Socket · epoll ET · eventfd · signalfd · sendfile · TCP_NODELAY · HTTP/1.1 · std::thread · mutex · condition_variable · CMake · Git · wrk

HTTP 服务器使用线程池、eventfd 和 sendfile；房间服务使用单线程 epoll 事件循环和自定义消息协议，不额外引入线程池。

## 已实现功能

### HTTP 服务器

- **事件驱动网络通信**：非阻塞 Socket、epoll ET、多客户端连接接入及读写事件处理。
- **HTTP 解析**：支持 HTTP/1.0 / HTTP/1.1 请求行、请求头及基于 Content-Length 的请求体解析，区分完整请求、不完整请求和格式错误。
- **基础请求处理**：GET / HEAD、Keep-Alive、HTTP 状态码和响应头生成。
- **静态资源服务**：文档根目录、默认首页、MIME 类型映射、查询字符串剥离及基础路径检查。
- **线程协作**：I/O 线程维护 Connection、客户端 Socket 和 epoll 状态；worker 处理请求，通过线程安全完成队列与 eventfd 返回结果。
- **连接身份校验**：使用 fd + connection_id 校验 worker 结果，防止 fd 复用后将旧连接的响应交给新连接。
- **同连接请求串行化**：后续请求保存在 conn.in，当前响应发送完成后再提交下一个请求。
- **静态文件传输**：worker 使用 open / fstat 获取文件描述符与大小，I/O 线程先 send 响应头，再 sendfile 文件正文。
- **部分发送处理**：保存未发送数据、文件偏移和剩余长度，发送至完成或 EAGAIN / EWOULDBLOCK。
- **正常静态资源 HEAD 响应**：不发送文件正文，Content-Length 保留对应 GET 请求的资源正文长度。
- **小响应延迟处理**：客户端 Socket 开启 TCP_NODELAY，消除本地测试中响应头与文件正文分开发送产生的约 41 ms 延迟。
- **信号与停止处理**：通过 signalfd 接收 SIGINT / SIGTERM，由 I/O 线程执行停止流程；忽略 SIGPIPE，发送失败通过返回值处理。
- **辅助能力**：线程池、阻塞队列、异步日志、空闲连接清理、配置文件解析及基础模块测试。

### TCP 房间服务

- **长连接通信**：同一 TCP 连接可以连续发送多条消息，不需要每次重新连接。
- **自定义消息协议**：采用“正文长度 + 消息类型 + 正文”，处理分段到达及一次接收多条消息的情况。
- **消息回显**：支持 Echo / EchoReply，用于验证基础通信。
- **加入与切换房间**：使用 Connection::room 保存归属，同一连接最多属于一个房间。
- **同房间广播**：服务器主动将消息发送给同房间成员，包含发送者自己，不向其他房间分发。
- **主动离房**：清除房间归属但保留 TCP 连接，离房后仍可回显和重新加入。
- **断线与重新连接**：关闭连接时移除对应状态，新连接默认未入房，不继承旧连接的房间。
- **发送积压限制**：每连接用户态待发送数据上限为 1 MiB；追加后会超过上限时关闭该连接。
- **基础半关闭处理**：对端停止发送后，尝试发送完已排队的回复，再关闭连接；不再为该连接加入新广播。
- **信号处理**：通过 signalfd 接收退出信号，结束事件循环后释放连接与相关 fd。

## 项目结构

```text
http_server/
├── include/
│   ├── tiny_http/
│   │   ├── async_logger.h
│   │   ├── blocking_queue.h
│   │   ├── config.h
│   │   ├── config_parser.h
│   │   ├── http_parser.h
│   │   ├── http_request.h
│   │   ├── http_response.h
│   │   ├── linux_server.h
│   │   ├── mime_types.h
│   │   └── thread_pool.h
│   └── tiny_room/
│       └── protocol.h          # 房间消息类型与编解码
├── src/
│   ├── main.cpp                # HTTP 服务器入口
│   ├── linux_server.cpp        # HTTP 事件循环与连接管理
│   ├── http_parser.cpp
│   ├── http_response.cpp
│   ├── mime_types.cpp
│   ├── thread_pool.cpp
│   ├── async_logger.cpp
│   ├── config_parser.cpp
│   └── room_server.cpp         # TCP 房间服务器及其入口
├── tests/
│   ├── parser_tests.cpp
│   ├── config_parser_tests.cpp
│   └── room_protocol_tests.cpp
├── www/
│   └── index.html
├── .gitignore
├── CMakeLists.txt
└── tiny_httpd.conf
```

本地压测文件 `www/bench_*.bin` 和原始结果目录 `bench-results/` 不提交 Git。

## 核心流程

### HTTP 请求处理

```text
客户端连接
    ↓
I/O 线程：accept4 → recv → conn.in
    ↓
检测完整请求，确认 processing == false
    ↓
processing = true → 提交线程池
    ↓
worker：解析请求 → 路径检查 → open / fstat → 构造 ResponseResult
    ↓
完成队列保存结果 → write(eventfd) 通知
    ↓
I/O 线程：读取结果 → 校验 fd + connection_id → 接管发送状态
    ↓
send 发送 conn.out
    ↓
存在文件正文时继续 sendfile
    ├── EAGAIN：保留状态，等待下一次 EPOLLOUT
    ├── 发送错误：关闭连接并释放该连接持有的资源
    └── 全部发送完成
          ├── Connection: close → 关闭连接
          └── Keep-Alive → processing = false
                            → 主动处理 conn.in 中的下一请求
```

普通错误响应的文本正文仍保存在 conn.out 中，通过 send 发送；并非所有响应都使用 sendfile。

### 房间消息处理

```text
客户端连接
    ↓
accept4 → 注册 epoll
    ↓
recv → conn.in → 消息解码
    ↓
按消息类型处理
    ├── Echo：返回原正文
    ├── JoinRoom：登记或切换房间
    ├── LeaveRoom：清除房间归属，保留连接
    └── RoomMessage：追加广播到同房间成员的 conn.out
    ↓
尝试非阻塞 send
    ├── 已发完：保留读事件关注
    └── EAGAIN：保留剩余数据，关注 EPOLLOUT
```

广播接收者不必先发请求。服务器为其排入消息并安排写事件，实现主动推送。

## 并发与状态管理

### HTTP：I/O 线程 + 工作线程池

I/O 线程管理连接表、连接缓冲区、客户端 Socket、epoll 事件及连接关闭。worker 不直接访问 Connection，只生成 ResponseResult。

线程间通过两个通道协作：

```text
completion_queue → 传递实际处理结果
eventfd          → 通知事件循环有完成结果
```

I/O 线程通过 try_pop 取出已有结果，队列为空时结束本轮处理，不在条件变量上等待。

每个连接分配独立 connection_id：

```text
旧连接：fd = 7，connection_id = 10
关闭
新连接：fd = 7，connection_id = 11
```

I/O 线程同时校验 fd 和 connection_id，识别并丢弃旧 worker 结果。

同一连接一次只处理一个请求，不同连接的业务任务可以由多个 worker 并行执行。当前响应发完后主动检查 conn.in，避免已经缓存的请求等待新的网络读事件。

### 房间服务：单线程事件循环

连接管理、消息解码、房间归属修改和广播分发都在同一线程中执行，不使用 HTTP 服务器的 worker、完成队列或 eventfd。

房间归属直接保存在 Connection::room 中。广播时遍历连接表，筛选同名房间成员；暂不引入独立房间管理框架。

发送数据追加到 conn.out，而不是覆盖已有内容。未半关闭的连接持续关注读事件，只有待发送数据尚未发完时才保留写事件关注。

广播遍历期间先收集需要关闭的连接，遍历结束后再删除，避免破坏当前遍历；发送者被关闭时，处理函数返回 false，调用者不再使用原连接引用。

## TCP 房间消息协议

### 消息格式

```text
┌──────────────────┬──────────────────┬──────────────────┐
│ 正文长度：4 字节  │ 消息类型：2 字节  │ 正文：N 字节     │
└──────────────────┴──────────────────┴──────────────────┘
```

固定头部为 6 字节：

- 正文长度不包含头部，整条消息长度为 `6 + N`。
- 正文长度使用 uint32_t，消息类型使用 uint16_t。
- 两个整数都使用网络字节序，即大端。
- 正文最大为 64 KiB，即 65,536 字节。
- 房间名限定为 1～32 字节，按字节数而不是字符数检查。

解码结果分为 Complete、Incomplete 和 TooLarge。不完整的数据保留在输入缓冲区；完整消息解析后只删除本条消息占用的字节，继续处理后续消息。

### 消息类型

| 编号 | 类型 | 方向 | 正文 |
| ---: | --- | --- | --- |
| 1 | `Echo` | 客户端 → 服务端 | 待回显数据 |
| 2 | `EchoReply` | 服务端 → 客户端 | 原样返回的数据 |
| 3 | `JoinRoom` | 客户端 → 服务端 | 房间名 |
| 4 | `JoinRoomReply` | 服务端 → 客户端 | 确认加入的房间名 |
| 5 | `RoomMessage` | 客户端 → 服务端 | 待广播数据 |
| 6 | `RoomBroadcast` | 服务端 → 房间成员 | 原样转发的数据 |
| 7 | `LeaveRoom` | 客户端 → 服务端 | 必须为空 |
| 8 | `LeaveRoomReply` | 服务端 → 客户端 | 刚离开的房间名；原本未入房则为空 |
| 255 | `Error` | 服务端 → 客户端 | 错误说明 |

### 房间行为

客户端首次连接时未加入任何房间。JoinRoom 成功后登记房间归属，再次 JoinRoom 会切换到新房间；非法房间名返回 Error，不改变原归属。

RoomMessage 只能在已入房状态发送。广播包含发送者自己，正文暂不附加用户身份或时间戳。

LeaveRoom 只清除房间归属，不关闭 TCP，也不清空此前已经排队的发送数据。离房后不再加入原房间的新广播，可以继续回显或重新入房。

连接断开后，房间归属随 Connection 的释放而消失。重新建立 TCP 连接不等于恢复原会话，必须重新加入房间。

正文超长或收到不支持的客户端消息类型时，当前实现关闭连接。

## epoll ET 处理

监听 Socket 和客户端 Socket 使用非阻塞模式。

| 操作 | 正常处理方式 |
| --- | --- |
| `accept4` | 循环接入，直到 EAGAIN / EWOULDBLOCK |
| `recv` | 循环读取并追加输入缓冲区，直到 EAGAIN / EWOULDBLOCK |
| `send` | 循环发送，直到用户态待发送数据为空或遇到 EAGAIN / EWOULDBLOCK |
| `sendfile` | 循环传输，直到文件正文发完或遇到 EAGAIN / EWOULDBLOCK |

HTTP 连接存在正在处理的请求时，进入读处理后仍继续接收并缓存数据，只是不提交第二个请求。

发送遇到 EAGAIN 时，不清空剩余数据，也不结束当前业务生命周期：

```text
当前暂时写不进去
    ↓
保留 conn.out 或文件发送位置
    ↓
等待下一次 EPOLLOUT
    ↓
从上次位置继续
```

只有响应全部发送完成后，HTTP 连接才进入关闭或处理下一请求的阶段。

## 信号与停止流程

### HTTP 服务器

程序在创建工作线程前屏蔽 SIGINT / SIGTERM，再创建 signalfd 并加入 epoll。

```text
Ctrl+C / SIGTERM
    ↓
signalfd 可读
    ↓
I/O 线程读取退出信号
    ↓
running_ = false
    ↓
退出事件循环
    ↓
stop()
```

清理顺序：

```text
停止线程池并等待 worker
    ↓
清理完成队列中尚未移交的文件 fd
    ↓
释放客户端连接及其文件 fd
    ↓
关闭监听 fd、eventfd、signalfd、epoll
    ↓
停止日志线程
```

HTTP 进程忽略 SIGPIPE，使 send / sendfile 的相关发送失败通过错误返回处理。

当前停止流程不保证将所有未完成响应发送完毕，停机可能中断正在传输的文件。

### 房间服务器

房间服务器同样通过 signalfd 接收 SIGINT / SIGTERM。退出事件循环后，由 RoomServer 析构函数释放连接和相关 fd。

发送使用 MSG_NOSIGNAL，发送失败时按返回值处理。房间服务不创建额外的工作线程或日志线程。

## 静态文件发送优化

此部分仅针对 HTTP 服务器。

### 优化前

```text
文件
    ↓
ifstream / ostringstream
    ↓
用户态字符串
    ↓
HTTP 响应序列化
    ↓
conn.out
    ↓
send
```

### 优化后

```text
HTTP 响应头 → conn.out → send
静态文件正文 → file_fd → sendfile → 客户端 Socket
```

文件正文不再完整读入用户态响应字符串。

Connection 保存 file_fd、file_offset 和 file_remaining，用于部分发送后的继续传输。文件发送完成、连接关闭或结果被丢弃时，释放对应文件 fd。

## 编译

环境要求：

- Linux，或 Windows 下的 WSL Linux 环境
- 支持 C++20 的 GCC / Clang
- CMake 3.16 或以上

```bash
git clone https://github.com/zheng-jiu/http_server.git
cd http_server

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

构建后生成：

```text
build/tiny_httpd
build/tiny_roomd
build/parser_tests
build/config_parser_tests
build/room_protocol_tests
```

网络实现依赖 Linux 的 epoll、eventfd、signalfd、sendfile 等接口，不能直接作为原生 Windows 网络程序运行。

## 运行

### HTTP 服务器

在仓库根目录执行：

```bash
./build/tiny_httpd
```

默认设置：

```text
配置文件：tiny_httpd.conf
监听地址：0.0.0.0:8080
静态目录：www/
```

查看响应：

```bash
curl -i http://127.0.0.1:8080/
```

位置参数依次覆盖端口、静态资源目录和工作线程数：

```bash
./build/tiny_httpd 8081 www 4
```

相对路径以启动时的工作目录为基准。默认监听所有 IPv4 网络接口，建议仅在本地或受控环境中运行。

### TCP 房间服务器

```bash
./build/tiny_roomd
```

预期输出：

```text
Room server: 127.0.0.1:9090
```

当前房间服务器固定监听本机回环地址的 9090 端口，不读取 tiny_httpd.conf。

默认情况下两个程序可以在不同终端同时运行。修改 HTTP 监听端口时，不要与房间服务器的 9090 冲突。

房间服务使用自定义二进制协议，不能直接用浏览器或 HTTP curl 请求访问。

### 房间服务快速体验

保持 tiny_roomd 运行，在另一终端使用 Python 3 执行：

```bash
python3 - <<'PY'
import socket
import struct

def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("响应未收完整，连接已关闭")
        data.extend(chunk)
    return bytes(data)

def exchange(sock, kind, text, expected_kind, expected_text):
    body = text.encode("utf-8")
    sock.sendall(struct.pack("!IH", len(body), kind) + body)

    size, reply_kind = struct.unpack("!IH", recv_exact(sock, 6))
    if size > 65536:
        raise RuntimeError("响应长度超限")

    reply = recv_exact(sock, size).decode("utf-8")
    if (reply_kind, reply) != (expected_kind, expected_text):
        raise RuntimeError(f"响应不符：{reply_kind}, {reply}")

    print(f"类型 {reply_kind}：{reply}")

with socket.create_connection(("127.0.0.1", 9090), timeout=3) as sock:
    exchange(sock, 1, "hello", 2, "hello")
    exchange(sock, 3, "lobby", 4, "lobby")
    exchange(sock, 5, "hello room", 6, "hello room")
    exchange(sock, 7, "", 8, "lobby")
    exchange(sock, 1, "still connected", 2, "still connected")

print("回显、入房、广播、离房及连接复用验证通过")
PY
```

该示例验证单个连接的完整操作流程；广播包含发送者，因此客户端可以收到自己发送的房间消息。多客户端隔离与重连验证见下方测试记录。

两个服务器均可在前台运行的终端中按 Ctrl+C 停止。

## HTTP 配置文件

tiny_httpd.conf 仅供 HTTP 服务器使用。

```nginx
server {
    listen 8080;
    root "www";

    worker_threads 4;
    keep_alive 15;
    max_events 1024;

    log_path "tiny_httpd.log";
    log_level info;

    max_body_size 1048576;
}
```

已接入主要运行流程：

| 配置项 | 作用 |
| --- | --- |
| `listen` | 监听端口 |
| `root` | 静态资源根目录 |
| `worker_threads` | 工作线程数量 |
| `keep_alive` | 空闲连接超时秒数 |
| `max_events` | 单次 epoll_wait 的事件数组容量，不是最大连接数 |
| `log_path` | 日志文件路径 |

可解析但尚未接入对应运行逻辑：

| 配置项 | 当前状态 |
| --- | --- |
| `log_level` | 尚未按配置控制日志过滤 |
| `max_body_size` | 尚未按配置限制 HTTP 请求体大小 |
| `rate_limit` | 尚未实现请求限流 |
| `server_name` | 尚未实现按域名分发 |
| `location` | 尚未接入请求路由 |
| `proxy_pass` | 尚未实现反向代理 |
| `expires` | 尚未实现缓存过期控制 |

当前 `-c` 参数与位置参数覆盖逻辑存在冲突，使用默认配置文件路径和上述位置参数。

房间协议的 64 KiB 正文上限是独立的协议限制，与 HTTP 配置中的 max_body_size 无关。

## 测试

### CTest 基础测试

Debug 构建后执行：

```bash
ctest --test-dir build --output-on-failure
```

当前有三个测试目标：

| 测试目标 | 主要覆盖内容 |
| --- | --- |
| `parser_tests` | HTTP 请求解析、不完整和非法请求、Content-Length 请求体、MIME 查询、基础响应序列化 |
| `config_parser_tests` | 配置解析、默认值、虚拟主机与 location 结构、错误处理、位置参数覆盖 |
| `room_protocol_tests` | 编解码、头部和正文分批到达、连续多条消息、超长正文声明 |

最近一次本地执行结果为 3 / 3 通过。

下面的客户端行为验证通过独立脚本手动执行，不等同于已加入 CTest 自动化测试。

### HTTP 功能验证

- 4 KiB、100 KiB、1 MiB 文件使用 curl 下载后，通过 cmp 验证内容一致。
- 1 MiB 文件 HEAD 请求返回 Content-Length: 1048576。
- 首页 HEAD 请求返回对应 index.html 的实际正文长度。
- 同一 curl 进程连续请求时复用 Keep-Alive 连接。
- 100 次请求、最多 20 个并行 curl 进程的基础检查通过。

### HTTP 异常连接与停止验证

- 使用 32 MiB 文件、64 KiB/s 限速和 2 秒超时，连续主动中断 3 次下载。
- sendfile 返回连接重置错误后，服务器仍运行，后续首页请求返回 HTTP 200。
- 文件传输过程中发送 SIGTERM，服务器通过 signalfd 接收退出信号并结束，退出码为 0。
- Ctrl+C / SIGINT 能够通过 signalfd 触发正常退出。

### 房间服务行为验证

| 场景 | 已观察到的结果 |
| --- | --- |
| 长连接回显 | 同一连接连续发送 hello、world，均收到正确回复 |
| 加入与切换 | 先加入 lobby，再切换到 arena，服务器返回对应确认 |
| 非法房间名 | 空房间名被拒绝，连接仍可回显 |
| 未入房发送消息 | 返回 Error，不执行房间广播 |
| 同房间广播 | A、B 在 lobby 时均收到广播，arena 中的 C 不收到 |
| 切换后的隔离 | B 切换到 arena 后，与 C 接收该房间广播，A 不收到 |
| 主动离房 | 返回离房确认，保留 TCP 连接，不再接收原房间的新广播 |
| 离房后重新加入 | 同一连接重新入房后恢复接收广播 |
| 成员直接断线 | A 断线后，B、C 仍能正常广播 |
| 重新建立连接 | 新连接默认未入房，回显正常，主动入房后才恢复广播 |

这些结果不代表所有并发、慢客户端、异常网络和长期稳定性场景均已验证。

## HTTP 压力测试

以下数据只属于 tiny_httpd 的静态文件传输测试，不代表 tiny_roomd 的广播性能。

### 测试环境

| 项目 | 设置 |
| --- | --- |
| 环境 | VMware Ubuntu 虚拟机 |
| 可用逻辑 CPU | 4 |
| HTTP worker | 4 |
| 工具 | wrk debian/4.1.0-4build2 |
| 地址 | 127.0.0.1:8080 |
| wrk 线程 | 4 |
| 并发连接 | 100 |
| 单轮时间 | 30 秒 |
| 文件大小 | 4 KiB / 100 KiB / 1 MiB |

服务端与 wrk 运行在同一虚拟机，结果包含回环网络、页缓存和虚拟机调度影响，不代表物理网卡或公网吞吐能力。

### 测试文件

首次创建测试文件；已有这些文件时无需重新生成：

```bash
dd if=/dev/zero of=www/bench_4k.bin bs=4K count=1
dd if=/dev/zero of=www/bench_100k.bin bs=100K count=1
dd if=/dev/zero of=www/bench_1m.bin bs=1M count=1
```

测试文件仅在本地使用，原始压测输出保存在 bench-results/，均不提交 Git。

### 压测命令

保持 HTTP 服务器运行，在另一个终端依次执行：

```bash
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_4k.bin
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_100k.bin
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_1m.bin
```

### 优化前后实测结果

下表保留最初基线与一组完整优化后复测结果，不是多轮平均值。

| 文件 | 版本 | Requests/sec | 平均延迟 | P99 | Transfer/sec |
| --- | --- | ---: | ---: | ---: | ---: |
| 4 KiB | 优化前 | 65,542.88 | 1.52 ms | 3.65 ms | 262.59 MB/s |
| 4 KiB | sendfile + TCP_NODELAY | 60,788.03 | 1.64 ms | 2.97 ms | 243.54 MB/s |
| 100 KiB | 优化前 | 24,467.31 | 3.99 ms | 10.02 ms | 2.34 GB/s |
| 100 KiB | sendfile + TCP_NODELAY | 35,012.17 | 2.78 ms | 6.59 ms | 3.34 GB/s |
| 1 MiB | 优化前 | 3,302.45 | 29.79 ms | 45.37 ms | 3.23 GB/s |
| 1 MiB | sendfile + TCP_NODELAY | 10,745.17 | 7.43 ms | 14.70 ms | 10.49 GB/s |

吞吐单位沿用 wrk 输出。这组数据来自 sendfile 优化阶段，并非之后每次代码更新都重新执行的结果。

### 测试观察

**4 KiB**

Requests/sec 从 65,542.88 降至 60,788.03，下降约 7.3%；平均延迟略升，P99 从 3.65 ms 降至 2.97 ms。小文件并非所有指标都获益。

**100 KiB**

Requests/sec 从 24,467.31 升至 35,012.17，提升约 43.1%；平均延迟从 3.99 ms 降至 2.78 ms，下降约 30.3%。

**1 MiB**

Requests/sec 从 3,302.45 升至 10,745.17，达到基线约 3.25 倍，即提升约 225%；平均延迟从 29.79 ms 降至 7.43 ms，下降约 75%。

本轮结果中，中大静态文件收益明显，小文件没有获得吞吐提升。目前保留统一的 sendfile 静态文件发送路径，未增加按文件大小切换发送方式的阈值策略。

## TCP_NODELAY 调优过程

此实验仅针对 HTTP 静态文件服务。

首次将发送改为：

```text
send 响应头 + sendfile 文件正文
```

后，4 KiB 场景出现稳定约 41 ms 的平均延迟：

```text
平均延迟约 40.97 ms
Requests/sec 约 2,437
```

重启服务器后复测仍可重现。给客户端 Socket 开启 TCP_NODELAY 后，平均延迟恢复到约 1.6～1.8 ms。

该对照实验与 Nagle 算法和延迟 ACK 交互造成的小响应延迟现象相符，但尚未通过抓包确认完整报文时序。

## 项目目的

通过实际完成：

```text
设计 → 编码 → 测试 → 发现问题 → 修改 → 对照验证
```

理解 Linux Socket、epoll、HTTP、自定义消息协议、线程协作、连接生命周期、信号处理、静态文件传输以及服务端主动推送。

HTTP 部分侧重线程协作与文件传输；房间服务侧重长连接、有状态消息处理和多客户端交互。优先保证实现可解释、行为可验证，再根据实际需求扩展。