# TinyHttpServer

一个基于 **C++20 + Linux** 实现的轻量级 HTTP/1.1 服务器，用于学习和实践 Linux 网络编程、HTTP 协议解析、多线程并发与服务器工程化开发。

项目采用非阻塞 Socket 和 epoll ET 事件循环。I/O 线程统一维护客户端连接状态、Socket 与 epoll 事件，工作线程负责请求处理，并通过线程安全完成队列与 eventfd 将处理结果返回事件循环。目前已实现静态资源服务、Keep-Alive、线程池、异步日志、配置文件解析以及基础的连接并发管理。

项目定位为持续迭代的学习项目，不作为可直接用于生产环境的成熟服务器。

## 技术栈

C++20 · Linux · TCP / Socket · epoll ET · eventfd · std::thread · mutex · condition_variable · CMake · Git

## 已实现功能

- **事件驱动网络通信**：使用非阻塞 Socket 与 epoll ET 实现监听、连接接入以及客户端读写事件处理。
- **HTTP 解析**：支持 HTTP/1.0 / HTTP/1.1 请求行、请求头以及基于 Content-Length 的请求 Body 解析，并区分完整请求、不完整请求与格式错误。
- **基础请求处理**：支持 GET / HEAD、Keep-Alive、HTTP 状态码和响应头生成。
- **静态资源服务**：支持文档根目录、默认首页、MIME 类型映射、查询字符串剥离及基础路径穿越检查。
- **线程池与任务队列**：使用线程池处理 HTTP 请求任务，并通过互斥锁和条件变量实现线程安全阻塞队列。
- **I/O 与业务线程解耦**：I/O 线程统一维护 Connection、Socket 和 epoll 状态，worker 不直接修改连接状态。
- **worker 完成通知**：worker 将 ResponseResult 写入线程安全完成队列，并通过 eventfd 唤醒 epoll 事件循环。
- **连接生命周期保护**：每条连接分配独立 connection_id，避免 fd 被复用后，旧 worker 结果误作用于新连接。
- **同连接请求串行化**：同一 TCP 连接一次只处理一个 HTTP 请求，后续请求保存在输入缓冲区，前一个响应发送完成后再继续处理。
- **非阻塞响应发送**：epoll ET 模式下循环 send，直到响应全部发送完成或遇到 EAGAIN / EWOULDBLOCK。
- **异步日志**：使用后台日志线程写入日志文件，支持时间戳和日志级别。
- **连接超时处理**：根据最近活动时间清理空闲 Keep-Alive 连接。
- **配置与构建**：支持配置文件解析、命令行参数覆盖、CMake 构建以及 HTTP / 配置解析测试。

> 当前仍有部分协议、异常处理和测试边界需要完善，例如 HEAD 响应的 Content-Length、请求大小限制、信号处理以及更完整的并发与异常连接测试。

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
├── CMakeLists.txt
└── tiny_httpd.conf
```

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
判断是否存在完整 HTTP 请求
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
处理静态资源
    ↓
构造 HTTP Response
    ↓
生成 ResponseResult
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
conn.out
    ↓
切换 EPOLLOUT
    ↓
循环 send
    ↓
全部发送完成 / EAGAIN
    ↓
Keep-Alive → processing = false
    ↓
继续处理 conn.in 中的下一请求

或

Connection: close
    ↓
关闭连接
```

### 并发模型

连接状态统一由 I/O 线程维护，包括：

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

worker 线程不直接操作 Connection，而只负责：

```text
HTTP 请求
    ↓
业务处理
    ↓
ResponseResult
```

处理结果通过：

```text
completion_queue
    +
eventfd
```

返回事件循环。

`completion_queue` 负责传递真正的响应数据，`eventfd` 只负责通知 epoll 有新的处理结果。

每条 Connection 还拥有独立的 `connection_id`：

```text
fd = 7, connection_id = 10
        ↓
连接关闭

fd = 7, connection_id = 11
```

即使 Linux 复用了相同的文件描述符，也能够识别并丢弃旧连接产生的过期 worker 结果。

## epoll ET 处理

监听 Socket、客户端 Socket 均使用边缘触发模式。

### 接收新连接

```text
EPOLLIN
    ↓
循环 accept4
    ↓
直到 EAGAIN / EWOULDBLOCK
```

### 读取请求

```text
EPOLLIN
    ↓
循环 recv
    ↓
数据追加到 conn.in
    ↓
直到 EAGAIN / EWOULDBLOCK
```

### 发送响应

```text
EPOLLOUT
    ↓
循环 send
    ↓
├── conn.out 为空
│       → 响应发送完成
│
└── EAGAIN / EWOULDBLOCK
        → 内核发送缓冲区暂时满，当前不可写
        → 保留剩余 conn.out
        → 等待下一次 EPOLLOUT 后继续发送
```

## 编译

环境要求：

- Linux，或 Windows 下的 WSL Linux 环境
- 支持 C++20 的 GCC / Clang
- CMake 3.16 或以上
- Make 或其他 CMake 支持的构建工具

```bash
git clone https://github.com/zheng-jiu/http_server.git
cd http_server

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

网络模块依赖 Linux 的 epoll、eventfd 等接口，不能直接作为原生 Windows 网络程序编译运行。

## 运行

在仓库根目录执行：

```bash
./build/tiny_httpd
```

默认读取：

```text
tiny_httpd.conf
```

默认监听：

```text
0.0.0.0:8080
```

默认静态资源目录：

```text
www/
```

浏览器访问：

```text
http://127.0.0.1:8080/
```

或者：

```bash
curl -i http://127.0.0.1:8080/
```

命令行位置参数可覆盖端口、资源目录和工作线程数，例如：

```bash
./build/tiny_httpd 9090 www 4
```

路径相对于程序启动时的工作目录解析。

默认监听所有 IPv4 网络接口，当前版本建议仅在本地或受控环境中运行。

## 配置文件

基础配置示例：

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

当前已接入主要运行流程的配置包括：

| 配置项 | 作用 |
| --- | --- |
| `listen` | 监听端口 |
| `root` | 静态资源根目录 |
| `worker_threads` | 工作线程数量 |
| `keep_alive` | Keep-Alive 空闲连接超时时间 |
| `max_events` | 单次 epoll_wait 使用的事件数组容量 |
| `log_path` | 日志文件路径 |

配置解析器还支持以下字段或结构，但相关运行逻辑尚未完全接入：

| 配置项 | 当前状态 |
| --- | --- |
| `log_level` | 可解析，尚未用于控制日志器运行时过滤级别 |
| `max_body_size` | 可解析，尚未用于限制实际 HTTP 请求体大小 |
| `rate_limit` | 可解析，尚未实现请求限流 |
| `server_name` | 可解析虚拟主机配置，尚未实现按域名分发 |
| `location` | 可解析，尚未接入请求路由 |
| `proxy_pass` | 可解析，尚未实现反向代理 |
| `expires` | 可解析，尚未实现缓存过期控制 |

配置文件无法读取或解析失败时，程序会输出警告并使用默认配置启动。

当前 `-c` 配置文件参数与位置参数覆盖逻辑仍存在冲突，后续会进一步整理命令行参数解析。

## 测试

Debug 构建完成后：

```bash
ctest --test-dir build --output-on-failure
```

当前测试主要覆盖：

- HTTP 基础请求解析
- 不完整 HTTP 请求
- 非法请求
- Content-Length 请求 Body
- MIME 类型查询
- HTTP 响应基础序列化
- 配置文件解析
- 默认配置
- 虚拟主机配置解析
- location 配置解析
- 配置错误处理
- 命令行位置参数覆盖

现有测试主要集中在协议解析和基础模块。

连接并发、异常断开以及长时间稳定性仍需要通过专门的并发测试、Sanitizer 和压力测试进一步验证。

## 当前改进重点

1. **连接关闭与异常路径**
   - 完善 EPOLLRDHUP、客户端异常断开和半关闭处理。
   - 继续检查连接关闭过程中各类边界情况。

2. **服务器停止与信号处理**
   - 当前 SIGINT / SIGTERM 信号处理流程仍需要进一步收敛。
   - 避免在信号处理函数中执行不适合异步信号环境的复杂逻辑。

3. **HTTP 协议边界**
   - 修正 HEAD 请求的 Content-Length。
   - 完善 Content-Length 异常值、请求头大小和请求体大小等边界。

4. **配置落地**
   - 将 max_body_size 等已经解析的配置真正接入请求处理流程。
   - 整理 `-c` 与位置参数解析冲突。

5. **静态资源安全**
   - 进一步完善路径规范化和目录边界检查。
   - 当前基础字符串检查不等同于完整的文件系统安全隔离。

6. **并发正确性验证**
   - 使用 ThreadSanitizer 检查潜在数据竞争。
   - 增加 Keep-Alive、多连接、异常断开和 fd 复用等测试。

7. **性能评估**
   - 使用 wrk 等工具进行可复现压力测试。
   - 使用 perf 等工具分析 CPU 热点。
   - 记录 QPS、延迟、错误率及测试环境。

## 项目目的

该项目主要用于深入理解并实践：

- Linux Socket 网络编程
- TCP 连接生命周期
- 非阻塞 I/O
- epoll LT / ET
- eventfd
- HTTP/1.1
- HTTP Keep-Alive
- 多线程与线程同步
- 线程池与生产者消费者模型
- I/O 与业务线程解耦
- 连接生命周期管理
- 文件描述符复用问题
- C++ 工程组织、测试与调试

项目优先完善正确性、并发模型和异常边界，在此基础上再逐步进行性能测试和功能扩展。