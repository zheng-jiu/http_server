# TinyHttpServer

一个基于 **C++20 + Linux** 实现的轻量级 HTTP/1.1 服务器，用于学习和实践 Linux 网络编程、HTTP 协议解析、多线程协作、连接生命周期管理与服务器性能优化。

项目采用非阻塞 Socket 和 epoll ET 事件循环。I/O 线程统一维护客户端连接、Socket 与 epoll 状态，工作线程负责请求处理，并通过线程安全完成队列与 eventfd 将结果返回事件循环。

静态文件传输使用 sendfile，避免将文件正文完整读入用户态字符串；客户端 Socket 开启 TCP_NODELAY，避免响应头与文件正文分开发送时出现明显的小响应延迟。

SIGINT / SIGTERM 通过 signalfd 接入 epoll 事件循环，由 I/O 线程统一触发服务器停止流程；同时忽略 SIGPIPE，使客户端异常断开时的发送失败通过错误码处理，避免单个连接导致服务器进程退出。

项目定位为持续迭代的学习项目，不作为可直接用于生产环境的成熟服务器。

## 技术栈

C++20 · Linux · TCP / Socket · epoll ET · eventfd · signalfd · sendfile · TCP_NODELAY · HTTP/1.1 · std::thread · mutex · condition_variable · CMake · Git · wrk

## 已实现功能

- **事件驱动网络通信**：使用非阻塞 Socket 与 epoll ET 实现监听、客户端连接接入和读写事件处理。
- **HTTP 解析**：支持 HTTP/1.0 / HTTP/1.1 请求行、请求头以及基于 Content-Length 的请求 Body 解析，并区分完整请求、不完整请求与格式错误。
- **基础请求处理**：支持 GET / HEAD、Keep-Alive、HTTP 状态码和响应头生成。
- **静态资源服务**：支持文档根目录、默认首页、MIME 类型映射、查询字符串剥离及基础路径穿越检查。
- **线程池与任务队列**：使用线程池处理 HTTP 请求任务，通过 mutex 与 condition_variable 实现线程安全阻塞队列。
- **I/O 与业务线程解耦**：I/O 线程统一维护 Connection、Socket 和 epoll 状态，worker 不直接操作连接状态。
- **worker 完成通知**：worker 将 ResponseResult 写入线程安全完成队列，并通过 eventfd 唤醒 epoll 事件循环。
- **连接生命周期保护**：每条连接分配独立 connection_id，避免 fd 复用后旧 worker 结果误作用于新连接。
- **同连接请求串行化**：同一 TCP 连接一次只处理一个请求，后续请求保存在 conn.in，当前响应发送完成后再继续处理。
- **ET 部分读写处理**：accept / recv / send / sendfile 均按照非阻塞 ET 模式处理至完成或 EAGAIN / EWOULDBLOCK。
- **sendfile 静态文件传输**：worker 使用 open / fstat 获取静态文件信息，由 I/O 线程使用 sendfile 发送文件正文。
- **HEAD 请求处理**：HEAD 不发送资源正文，但 Content-Length 保留对应资源的实际正文大小。
- **TCP_NODELAY**：解决响应头与 sendfile 文件正文分开发送时，小响应出现明显延迟的问题。
- **安全信号处理**：屏蔽 SIGINT / SIGTERM，并通过 signalfd 将退出信号接入 epoll，由 I/O 线程统一触发服务器停止流程。
- **异常发送保护**：忽略 SIGPIPE，使客户端异常断开时的发送失败通过错误码处理，避免单个连接导致服务器进程退出。
- **异步日志**：使用后台线程写入日志文件。
- **连接超时处理**：根据最近活动时间清理空闲 Keep-Alive 连接。
- **配置与构建**：支持配置文件解析、命令行参数覆盖、CMake 构建以及基础模块测试。

## 项目结构

```text
http_server/
├── include/
│   └── tiny_http/
│       ├── async_logger.h
│       ├── blocking_queue.h
│       ├── config.h
│       ├── config_parser.h
│       ├── http_parser.h
│       ├── http_request.h
│       ├── http_response.h
│       ├── linux_server.h
│       ├── mime_types.h
│       └── thread_pool.h
│
├── src/
│   ├── main.cpp
│   ├── linux_server.cpp
│   ├── http_parser.cpp
│   ├── http_response.cpp
│   ├── mime_types.cpp
│   ├── thread_pool.cpp
│   ├── async_logger.cpp
│   └── config_parser.cpp
│
├── tests/
│   ├── parser_tests.cpp
│   └── config_parser_tests.cpp
│
├── www/
│   └── index.html
├── CMakeLists.txt
└── tiny_httpd.conf
```

本地压测使用的 `bench_*.bin` 与 wrk 原始输出文件不提交 Git。

## 核心流程

```text
客户端连接
    ↓
I/O 线程
accept4
    ↓
非阻塞 recv
    ↓
conn.in
    ↓
检测完整 HTTP 请求
    ↓
processing = true
    ↓
提交线程池
    ↓
────────────────────────
worker 线程
    ↓
解析 HTTP 请求
    ↓
路径检查
    ↓
open / fstat 静态文件
    ↓
构造 ResponseResult
    ↓
completion_queue
    ↓
write(eventfd)
    ↓
────────────────────────
I/O 线程被 epoll 唤醒
    ↓
读取完成队列
    ↓
校验 fd + connection_id
    ↓
接管响应及文件发送状态
    ↓
EPOLLOUT
    ↓
send HTTP 响应头
    ↓
sendfile 静态文件正文
    ↓
全部发送完成 / EAGAIN
    ↓
Keep-Alive → processing = false
    ↓
主动检查 conn.in 中的下一请求

或

Connection: close
    ↓
关闭连接并释放资源
```

## 并发模型

连接状态由 I/O 线程统一管理，包括：

```text
connections_
Connection
client_fd
conn.in
conn.out
processing
epoll_ctl
close_connection
```

worker 不直接访问 Connection，而只负责：

```text
HTTP 请求
    ↓
请求处理
    ↓
ResponseResult
```

结果通过：

```text
completion_queue
    +
eventfd
```

返回事件循环。

其中：

- `completion_queue` 负责传递实际处理结果。
- `eventfd` 只负责通知 epoll 有新的完成结果。
- I/O 线程通过非阻塞 `try_pop()` 获取结果，不会因为完成队列为空而阻塞事件循环。

每条连接拥有独立的 `connection_id`：

```text
旧连接：
fd = 7
connection_id = 10

关闭后，新连接复用 fd：

fd = 7
connection_id = 11
```

即使 Linux 复用了相同文件描述符，也可以识别并丢弃旧连接产生的过期 worker 结果。

同一连接内部请求串行处理，不同连接之间仍可由多个 worker 并行处理。

## epoll ET 处理

### 新连接

```text
EPOLLIN
    ↓
循环 accept4
    ↓
直到 EAGAIN / EWOULDBLOCK
```

### 请求读取

```text
EPOLLIN
    ↓
循环 recv
    ↓
追加到 conn.in
    ↓
直到 EAGAIN / EWOULDBLOCK
```

即使当前连接已经存在正在处理的请求，也继续将内核接收缓冲区读取到 EAGAIN，只是不继续提交第二个请求。

### HTTP 响应头发送

```text
EPOLLOUT
    ↓
循环 send
    ↓
├── conn.out 为空
│       → 继续文件正文发送
│
└── EAGAIN / EWOULDBLOCK
        → 当前不可写
        → 保留剩余 conn.out
        → 等待下一次 EPOLLOUT
```

### 静态文件正文发送

```text
sendfile(
    client_fd,
    file_fd,
    &file_offset,
    file_remaining
)
    ↓
├── 文件发送完成
│       → 释放 file_fd
│
└── EAGAIN / EWOULDBLOCK
        → 保存 offset 与 remaining
        → 等待下一次 EPOLLOUT
```

## 信号与停止流程

SIGINT 和 SIGTERM 不再由异步信号处理函数直接执行复杂清理。

程序启动时先屏蔽退出信号，再通过 signalfd 将它们转换为可以由 epoll 监听的文件描述符事件：

```text
Ctrl+C / SIGTERM
    ↓
signalfd
    ↓
EPOLLIN
    ↓
I/O 线程读取退出信号
    ↓
running = false
    ↓
退出事件循环
    ↓
stop()
```

停止流程中：

```text
停止线程池并等待 worker
    ↓
清理完成队列中尚未移交的文件 fd
    ↓
释放客户端连接及其文件 fd
    ↓
关闭 listen_fd
    ↓
关闭 completion_fd
    ↓
关闭 signal_fd
    ↓
关闭 epoll_fd
    ↓
停止日志线程
```

同时忽略 SIGPIPE，使 `send` / `sendfile` 面对已经断开的客户端时通过错误返回处理，而不是直接终止整个服务器进程。

## 静态文件发送优化

### 优化前

```text
文件
    ↓
ifstream
    ↓
ostringstream / std::string
    ↓
ResponseResult
    ↓
conn.out
    ↓
send
```

文件正文会完整进入用户态字符串。

### 优化后

```text
HTTP 响应头
    ↓
conn.out
    ↓
send

静态文件正文
    ↓
file_fd
    ↓
sendfile
    ↓
client socket
```

文件正文不再完整复制进用户态响应字符串。

Connection 保存：

```text
file_fd
file_offset
file_remaining
```

用于非阻塞部分发送与后续 EPOLLOUT 继续传输。

## 编译

环境要求：

- Linux，或 Windows 下的 WSL Linux 环境
- 支持 C++20 的 GCC / Clang
- CMake 3.16 或以上

```bash
git clone https://github.com/zheng-jiu/http_server.git
cd http_server

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

网络模块依赖 Linux 的 epoll、eventfd、signalfd、sendfile 等接口，不能直接作为原生 Windows 网络程序编译运行。

## 运行

在仓库根目录：

```bash
./build/tiny_httpd
```

默认：

```text
配置文件：tiny_httpd.conf
监听地址：0.0.0.0:8080
静态目录：www/
```

访问：

```bash
curl -i http://127.0.0.1:8080/
```

位置参数可覆盖端口、静态资源目录和工作线程数：

```bash
./build/tiny_httpd 9090 www 4
```

默认监听所有 IPv4 网络接口，当前版本建议仅在本地或受控环境运行。

## 配置文件

基础配置：

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
| `keep_alive` | Keep-Alive 空闲连接超时 |
| `max_events` | 单次 epoll_wait 使用的事件数组容量 |
| `log_path` | 日志文件路径 |

可以解析但尚未完全接入对应运行逻辑：

| 配置项 | 当前状态 |
| --- | --- |
| `log_level` | 可解析，尚未控制运行时日志过滤 |
| `max_body_size` | 可解析，尚未限制实际请求体大小 |
| `rate_limit` | 可解析，尚未实现请求限流 |
| `server_name` | 可解析，尚未实现虚拟主机分发 |
| `location` | 可解析，尚未接入请求路由 |
| `proxy_pass` | 可解析，尚未实现反向代理 |
| `expires` | 可解析，尚未实现缓存过期控制 |

当前 `-c` 配置文件参数与位置参数覆盖逻辑仍存在冲突。

## 测试

Debug 构建后：

```bash
ctest --test-dir build --output-on-failure
```

当前测试主要覆盖：

- HTTP 请求解析
- 不完整请求
- 非法请求
- Content-Length 请求 Body
- MIME 类型查询
- HTTP 基础响应序列化
- 配置文件解析
- 默认配置
- 虚拟主机与 location 配置解析
- 配置错误处理
- 命令行位置参数覆盖

sendfile 修改后的功能验证：

- CTest：2 / 2 通过。
- 4 KiB、100 KiB、1 MiB 文件使用 curl 下载后，通过 cmp 验证内容一致。
- 1 MiB 文件 HEAD 请求返回 `Content-Length: 1048576`。
- 首页 HEAD 请求返回对应 index.html 的实际正文大小。

异常连接与停止流程验证：

- 使用 32 MiB 静态文件、64 KiB/s 限速和 2 秒客户端超时，连续主动中断 3 次下载。
- 服务端 sendfile 返回连接重置错误后没有退出，随后首页请求仍正常返回 HTTP 200。
- 文件传输过程中发送 SIGTERM，服务器通过 signalfd 收到退出信号并正常结束。
- SIGTERM 停止测试中服务器退出码为 0。
- Ctrl+C / SIGINT 同样能够通过 signalfd 触发正常退出。

这些测试不能代表所有并发、异常连接和长期稳定性场景均已验证。

## 压力测试

### 测试环境

| 项目 | 设置 |
| --- | --- |
| 环境 | VMware Ubuntu 虚拟机 |
| 逻辑 CPU | 4 |
| Server worker | 4 |
| 工具 | wrk debian/4.1.0-4build2 |
| 地址 | 127.0.0.1:8080 |
| wrk threads | 4 |
| 并发连接 | 100 |
| 单轮时间 | 30 秒 |
| 文件 | 4 KiB / 100 KiB / 1 MiB |

服务端与 wrk 运行在同一虚拟机，结果包含回环网络、页缓存和虚拟机调度影响，仅用于同环境下的版本对比。

### 测试文件

```bash
dd if=/dev/zero of=www/bench_4k.bin bs=4K count=1
dd if=/dev/zero of=www/bench_100k.bin bs=100K count=1
dd if=/dev/zero of=www/bench_1m.bin bs=1M count=1
```

这些文件仅用于本地测试，不提交 Git。

### 压测命令

```bash
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_4k.bin
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_100k.bin
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/bench_1m.bin
```

### 优化前后实测结果

下表采用最初基线，以及保存最终代码后重新执行的一组完整优化后测试。

| 文件 | 版本 | Requests/sec | Avg Latency | P99 | Transfer/sec |
| --- | --- | ---: | ---: | ---: | ---: |
| 4 KiB | 优化前 | 65,542.88 | 1.52 ms | 3.65 ms | 262.59 MB/s |
| 4 KiB | sendfile + TCP_NODELAY | 60,788.03 | 1.64 ms | 2.97 ms | 243.54 MB/s |
| 100 KiB | 优化前 | 24,467.31 | 3.99 ms | 10.02 ms | 2.34 GB/s |
| 100 KiB | sendfile + TCP_NODELAY | 35,012.17 | 2.78 ms | 6.59 ms | 3.34 GB/s |
| 1 MiB | 优化前 | 3,302.45 | 29.79 ms | 45.37 ms | 3.23 GB/s |
| 1 MiB | sendfile + TCP_NODELAY | 10,745.17 | 7.43 ms | 14.70 ms | 10.49 GB/s |

### 测试观察

**4 KiB**

```text
Requests/sec：65,542.88 → 60,788.03
变化约 -7.3%
```

小文件吞吐没有因为 sendfile 获得提升，但 P99 从 3.65 ms 降至 2.97 ms。

**100 KiB**

```text
Requests/sec：24,467.31 → 35,012.17
提升约 43.1%

Avg Latency：3.99 ms → 2.78 ms
下降约 30.3%
```

**1 MiB**

```text
Requests/sec：3,302.45 → 10,745.17
达到基线约 3.25 倍，提升约 225%

Avg Latency：29.79 ms → 7.43 ms
下降约 75%
```

sendfile 对中大静态文件的收益明显，小文件则没有获得吞吐提升。

目前没有为了追回 4 KiB 场景的少量性能差异而增加“小文件普通 send、大文件 sendfile”的双发送路径，以保持实现简单。

## TCP_NODELAY 调优过程

首次改为：

```text
send 响应头
+
sendfile 文件正文
```

后，4 KiB 压测出现稳定的约 41 ms 平均延迟：

```text
Avg Latency ≈ 40.97 ms
Requests/sec ≈ 2,437
```

服务器重启后重新测试仍能稳定复现。

随后给客户端 Socket 开启 TCP_NODELAY，4 KiB 延迟恢复至约 1.6～1.8 ms。

该对照实验与 Nagle 算法和延迟 ACK 交互造成的小响应延迟现象相符，但当前尚未通过 tcpdump / Wireshark 抓包进一步确认完整报文时序。

## 项目目的

通过实际实现：

```text
设计
→ 编码
→ 测试
→ 发现问题
→ 定位原因
→ 修改
→ 对照复测
```

理解 Linux Socket、epoll、HTTP、多线程协作、连接生命周期、信号处理以及静态文件传输。

项目优先保证正确性和可解释性，再根据真实测量结果进行性能优化，避免为了增加功能而增加不必要的复杂度。