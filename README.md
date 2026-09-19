# TinyHttpServer

一个基于 **C++20 + Linux** 实现的轻量级 HTTP/1.1 服务器，用于学习和实践 Linux 网络编程、HTTP 协议解析、多线程并发与服务器工程化开发。

项目采用非阻塞 Socket 和 epoll ET 事件循环，结合线程池处理请求，已实现静态资源服务、Keep-Alive、异步日志和配置文件解析等基础功能。目前仍在迭代连接并发访问、生命周期及发送流程，定位为学习项目，而非可直接用于生产环境的成熟服务器。

## 技术栈

C++20 · Linux · TCP / Socket · epoll ET · std::thread / mutex / condition_variable · CMake · Git

## 已实现功能

- **事件驱动网络通信**：非阻塞监听与客户端 Socket、epoll 边缘触发、多客户端连接管理。
- **HTTP 解析**：HTTP/1.0 / HTTP/1.1 请求行和请求头解析，按 Content-Length 提取请求 Body，并区分完整请求、不完整请求与格式错误。
- **基础请求处理**：GET / HEAD、Keep-Alive、响应状态码与响应头生成；解析器可读取其他方法，但业务处理目前仅支持 GET / HEAD。
- **静态资源服务**：文档根目录、默认首页、MIME 类型映射、查询字符串剥离及基础路径检查。
- **线程池与任务队列**：使用互斥锁和条件变量实现阻塞任务队列，由工作线程执行请求处理任务。
- **异步日志**：后台线程批量写入文件，提供时间戳与日志级别。
- **连接超时处理**：基于最近活动时间检查空闲连接。
- **配置与构建**：配置文件解析、位置参数覆盖、CMake 构建以及 HTTP / 配置解析测试。

> 当前功能仍有边界待完善，例如 HEAD 响应的 Content-Length、ET 模式下的部分写处理及同一连接的请求顺序，详见下方“当前改进重点”。

## 项目结构

```text
http_server/
├── include/
│   └── tiny_http/          # HTTP、网络、线程池、日志与配置模块头文件
├── src/
│   ├── main.cpp           # 程序入口与配置加载
│   ├── linux_server.cpp   # epoll 事件循环、连接管理与静态资源处理
│   ├── http_parser.cpp    # HTTP 请求解析
│   ├── http_response.cpp  # HTTP 响应构造
│   ├── mime_types.cpp     # MIME 类型映射
│   ├── thread_pool.cpp    # 工作线程池
│   ├── async_logger.cpp   # 异步日志
│   └── config_parser.cpp  # 配置词法与语法解析
├── tests/
│   ├── parser_tests.cpp
│   └── config_parser_tests.cpp
├── www/                   # 默认静态资源目录
├── CMakeLists.txt
└── tiny_httpd.conf
```

## 核心流程

```text
客户端连接
    ↓
主线程：accept → 非阻塞 recv → 缓冲请求并判断完整性
    ↓
线程池：解析请求 → 处理静态资源 → 构造响应
    ↓
主线程：收到 EPOLLOUT → send
    ↓
保持连接 / 关闭连接
```

网络事件由主线程处理，业务任务由线程池执行。当前工作线程仍会直接访问连接容器和连接状态，因此尚未形成严格的单线程连接状态管理；相关并发边界是后续改进重点。

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

示例使用 Debug 构建，以保留现有测试中的 assert 检查。网络模块依赖 Linux 的 epoll 等接口，不能直接作为原生 Windows 网络程序编译运行。

## 运行

在仓库根目录执行：

```bash
./build/tiny_httpd
```

默认读取当前工作目录下的 `tiny_httpd.conf`，监听 `0.0.0.0:8080`，静态资源目录为 `www/`。打开：

```text
http://127.0.0.1:8080/
```

也可以使用 curl 查看响应：

```bash
curl -i http://127.0.0.1:8080/
```

位置参数依次覆盖端口、资源目录和工作线程数，例如：

```bash
./build/tiny_httpd 9090 www 4
```

路径相对于程序启动时的工作目录解析。建议在本地或受控环境中运行；默认监听所有 IPv4 网络接口，当前版本不建议直接对公网提供服务。

## 配置文件

以下是已接入服务器运行流程的基础配置：

```nginx
server {
    listen 8080;
    root "www";
    worker_threads 4;
    keep_alive 15;
    max_events 1024;
    log_path "tiny_httpd.log";
}
```

- `listen`：监听端口。
- `root`：静态资源根目录。
- `worker_threads`：工作线程数量。
- `keep_alive`：空闲连接超时秒数。
- `max_events`：单次 epoll_wait 使用的事件数组容量，并非最大连接数。
- `log_path`：日志文件路径。

配置解析器还支持下列字段或结构，但它们**尚未接入相应运行逻辑**，不应视为已实现的服务能力：

| 配置项 | 当前状态 |
| --- | --- |
| `log_level` | 可解析；尚未传入日志器以控制运行时过滤级别 |
| `max_body_size` | 可解析；尚未用于限制请求体大小 |
| `rate_limit` | 可解析；尚未实现请求限流 |
| `server_name` | 可解析虚拟主机配置；尚未实现按域名分发 |
| `location`、`proxy_pass`、`expires` | 可解析并保存配置；尚未实现对应路由、反向代理与缓存过期策略 |

配置文件无法读取或解析失败时，入口程序会输出提示并使用默认配置。当前 `-c` 参数与位置参数解析存在冲突，暂以默认配置文件路径和上述位置参数用法为准。

## 测试

完成 Debug 构建后：

```bash
cd build
ctest --output-on-failure
```

现有测试主要覆盖：

- HTTP 请求解析：基础 GET、未收完整的请求、非法目标路径以及 Content-Length 请求体。
- MIME 类型查询与基础响应序列化。
- 配置解析：基础指令、注释、默认值、虚拟主机与 location 结构。
- 配置错误处理、文件不存在以及位置参数覆盖。

这些是解析与基础模块测试，不代表服务器的并发正确性、长时间稳定性或压力测试已经验证。

## 当前改进重点

1. **连接并发与生命周期**：收敛主线程和工作线程对连接容器、读写缓冲区的访问，处理连接关闭与文件描述符复用等边界。
2. **同一连接的请求顺序**：避免多个任务并行处理同一连接时发生响应覆盖或乱序。
3. **非阻塞发送流程**：完善部分写、EAGAIN 和异常路径处理，保证 ET 模式下响应能够持续发送。
4. **HTTP 与路径边界**：修正 HEAD 的 Content-Length，补充协议边界和静态资源路径校验；基础字符串检查不等同于完整的目录安全隔离。
5. **配置落地与验证**：修正 `-c` 与位置参数的冲突，逐步接入请求体限制等已解析配置。
6. **测试与性能评估**：补充连接、并发及异常路径测试，再开展 ASan / TSan 检查和可复现的压力测试。

## 项目目的

通过实现一个可读、可逐步验证的 HTTP 服务器，理解从 Socket、事件循环、协议解析到线程协作、日志与配置管理的完整链路。优先完善正确性和边界处理，再评估性能并扩展功能。
