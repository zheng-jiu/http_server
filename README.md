# TinyHttpServer

基于 C++20 和 Linux 实现的 HTTP 服务器，支持静态资源访问、多后端反向代理、轮询负载均衡与故障冷却恢复。

采用单 Reactor + 工作线程池架构：I/O 线程通过 epoll 管理客户端与后端连接，工作线程处理静态文件请求，异步日志线程负责日志写入。

## 功能

### HTTP 静态资源服务

- 解析 HTTP/1.0、HTTP/1.1 请求行、请求头和基于 Content-Length 的请求体。
- 支持 GET、HEAD、Keep-Alive、默认首页和 MIME 类型识别。
- 使用 sendfile 发送静态文件正文，记录文件偏移和剩余长度，处理部分发送。
- 同一客户端连接内串行处理请求，当前响应结束后继续处理已缓存的请求。
- 校验请求头大小、请求体大小、Content-Length 格式及重复声明。
- 工作线程池采用有界任务队列，排队任务达到上限时返回 503。

### 多后端反向代理与负载均衡

- 根据 location 规则进行最长路径前缀匹配，每条规则可以配置多个后端。
- 使用 Round Robin 轮询选择后端，跳过仍在冷却中的节点。
- 后端连接或传输故障、交互超时后进入 10 秒冷却，到期后由实际请求触发恢复尝试。
- 使用非阻塞 connect、send、recv 完成后端连接与请求转发。
- 支持 GET、HEAD，保留原请求路径和查询参数。
- 重写 Host，过滤逐跳头字段，保留多条 Set-Cookie 等重复响应头。
- 向后端发送 HTTP/1.0 短连接请求，接收定长响应或通过关闭连接结束的响应。
- 单独处理 HEAD、204、304 等无正文响应。
- 收齐后端响应后重新组织响应头，通过现有客户端发送流程返回。
- 后端连接或传输失败、响应格式错误或定长正文截断时返回 502，后端交互超时返回 504。
- 全部后端处于冷却状态时返回 503。

### 网络与连接管理

- 使用非阻塞 Socket 和 epoll ET 处理网络事件。
- 发送遇到 EAGAIN 时保存进度，等待后续可写事件继续发送。
- 使用完成队列和 eventfd 将工作线程结果交回 I/O 线程。
- 使用 fd + connection_id 校验工作线程结果与客户端连接的对应关系。
- 分别处理后端交互超时、客户端发送超时和客户端空闲超时。
- 使用 signalfd 接收 SIGINT、SIGTERM，并有序释放线程、连接和文件描述符。
- 客户端 Socket 开启 TCP_NODELAY。

## 技术栈

C++20、Linux Socket、epoll、eventfd、signalfd、sendfile、TCP_NODELAY、std::thread、mutex、condition_variable、CMake。

## 编译与运行

### 环境

- Linux 或 WSL2。
- 支持 C++20 的 GCC / Clang。
- CMake 3.16 及以上。
- curl 用于访问服务，Python 3 可用于启动示例后端。

### 编译

```bash
git clone https://github.com/zheng-jiu/http_server.git
cd http_server

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

### 启动 HTTP 服务器

在仓库根目录执行：

```bash
./build/tiny_httpd
```

默认读取 `tiny_httpd.conf`，监听 `0.0.0.0:8080`，静态资源目录为 `www/`。

访问首页：

```bash
curl --noproxy '*' -i http://127.0.0.1:8080/
```

通过位置参数覆盖端口、静态资源目录和工作线程数：

```bash
./build/tiny_httpd 8081 www 4
```

通过 `-c` 指定配置文件：

```bash
./build/tiny_httpd -c tiny_httpd.conf
```

配置文件与位置参数可以组合使用，位置参数优先：

```bash
./build/tiny_httpd -c custom.conf 8082 ./public 8
```

相对路径以进程启动时的工作目录为基准。前台运行时按 `Ctrl+C` 停止服务器。

## 配置

`tiny_httpd.conf` 示例：

```nginx
server {
    listen 8080;
    root "www";

    worker_threads 4;
    keep_alive 15;
    max_events 1024;

    log_path "tiny_httpd.log";

    max_body_size 1048576;
    max_pending_tasks 256;
    send_timeout 15;

    location /api/ {
        proxy_pass "http://127.0.0.1:9001";
        proxy_pass "http://127.0.0.1:9002";
    }
}
```

### 已接入运行流程的配置

| 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `listen` | `8080` | HTTP 监听端口 |
| `root` | `www` | 静态资源根目录 |
| `worker_threads` | `4` | 静态请求工作线程数 |
| `keep_alive` | `15` | 客户端空闲超时，单位为秒 |
| `max_events` | `1024` | 单次 epoll_wait 最多接收的就绪事件记录数 |
| `log_path` | `tiny_httpd.log` | 日志文件路径 |
| `max_body_size` | `1048576` | 请求体大小上限，单位为字节 |
| `max_pending_tasks` | `256` | 工作线程池最大排队任务数 |
| `send_timeout` | `15` | 向客户端发送时连续无进展的超时秒数 |
| `location` | 空 | 按请求路径匹配规则 |
| `proxy_pass` | 空 | 后端地址；同一 location 内可重复配置多个地址，按轮询顺序选择 |

配置解析器还保留了 `log_level`、`rate_limit`、`server_name`、location 内的 `root` 和 `expires` 语法，这些字段目前仅完成解析，对应功能尚未接入运行流程。

### 代理路由规则

匹配时使用去掉查询参数后的请求路径，多条规则同时匹配时选择路径最长的一条。

例如，同时存在 `/api/` 和 `/api/admin/` 时，`/api/admin/users` 选择 `/api/admin/`。

转发时保留完整的原请求路径和查询参数：

| 客户端请求 | 后端收到的请求路径 |
| --- | --- |
| `/api/users` | `/api/users` |
| `/api/users?page=1` | `/api/users?page=1` |

后端地址采用以下格式：

```text
http://IPv4:端口
```

例如：

```text
http://127.0.0.1:9001
```

地址显式填写端口，端口后省略路径和末尾斜杠。

## 反向代理运行示例

本示例使用两个后端，分别监听 9001、9002，由 TinyHttpServer 在 8080 端口统一接收请求。

确认配置中的 `/api/` 规则包含以下两个地址：

```nginx
location /api/ {
    proxy_pass "http://127.0.0.1:9001";
    proxy_pass "http://127.0.0.1:9002";
}
```

同一 location 内重复填写 proxy_pass，可以添加多个后端。配置修改后重启 TinyHttpServer 生效。

### 启动两个后端

终端 1：准备并启动后端 A。

```bash
mkdir -p /tmp/tiny-http-backend-a/api

printf 'A' > /tmp/tiny-http-backend-a/api/node.txt

python3 -m http.server 9001 \
    --bind 127.0.0.1 \
    --directory /tmp/tiny-http-backend-a
```

终端 2：准备并启动后端 B。

```bash
mkdir -p /tmp/tiny-http-backend-b/api

printf 'B' > /tmp/tiny-http-backend-b/api/node.txt

python3 -m http.server 9002 \
    --bind 127.0.0.1 \
    --directory /tmp/tiny-http-backend-b
```

### 启动代理并访问

终端 3：在仓库根目录启动 TinyHttpServer。

```bash
./build/tiny_httpd -c tiny_httpd.conf
```

终端 4：连续发送四个请求。

```bash
for i in 1 2 3 4; do
    printf '第 %s 次：' "$i"
    curl --noproxy '*' -sS --max-time 20 \
        -w ' HTTP %{http_code}\n' \
        http://127.0.0.1:8080/api/node.txt
done
```

刚启动代理且按上述顺序请求时，输出如下：

```text
第 1 次：A HTTP 200
第 2 次：B HTTP 200
第 3 次：A HTTP 200
第 4 次：B HTTP 200
```

### 观察故障摘除与恢复

1. 在终端 1 按 Ctrl+C，停止后端 A。
2. 在终端 4 再次执行四次请求命令。第一次选中 A 的请求返回 502，之后在冷却期间请求交给 B。
3. 在终端 1 重新执行后端 A 的启动命令。
4. 在终端 4 执行 `sleep 11`，等待冷却结束，再次发送请求，观察 A、B 恢复交替返回。

停止两个后端后，访问各节点的请求会分别触发 502 并记录冷却。当两个节点都处于冷却状态时，新请求返回 503。

### 访问静态资源

静态首页继续使用相同入口：

```bash
curl --noproxy '*' -i http://127.0.0.1:8080/
```

请求 `/api/node.txt` 时，由选中的后端提供内容；请求 `/` 时，由 TinyHttpServer 提供本地静态首页。

## 架构与请求流程

```mermaid
flowchart TD
    A["客户端"] --> B["I/O 线程：接收、解析与路由"]
    B -->|静态资源| C["工作线程：打开文件、构造响应"]
    C --> D["完成队列与 eventfd"]
    D --> E["I/O 线程：发送客户端响应"]
    B -->|代理请求| F["I/O 线程：连接后端、转发与接收"]
    F --> E
    E --> A
```

### 静态文件请求

1. I/O 线程接收数据，解析出一个完整请求。
2. 请求交给工作线程，客户端连接标记为处理中。
3. 工作线程完成路径检查、open、fstat 和响应头构造。
4. 工作线程把结果放入完成队列，通过 eventfd 通知 I/O 线程。
5. I/O 线程校验 fd 和 connection_id，接管响应与文件描述符。
6. 先使用 send 发送响应头，再使用 sendfile 发送文件正文。
7. 响应完成后，根据连接状态关闭连接或继续处理下一请求。

Connection 保存文件描述符、文件偏移和剩余长度。发送遇到 EAGAIN 时保留这些状态，后续可写事件到来后继续传输。

### 代理请求

1. I/O 线程解析请求，匹配包含 proxy_pass 的 location。
2. 按轮询顺序选择后端，跳过仍在冷却中的节点；全部节点处于冷却状态时返回 503。
3. 创建非阻塞后端 Socket，发起 connect，并在 Upstream 中保存选中的后端地址。
4. 连接处于 EINPROGRESS 状态时，等待就绪事件，通过 SO_ERROR 读取连接结果。
5. 发送重新组织的 HTTP 请求，保存部分发送的进度。
6. 请求发完后切换为关注后端可读事件。
7. 解析后端响应头，判断正文结束条件并累积响应数据。
8. 收齐响应后关闭后端 Socket，把响应交给客户端发送流程。
9. 客户端响应发送完成后关闭连接。

每个代理请求使用独立的后端连接。代理采用完整响应缓冲方式，静态资源请求继续支持 Keep-Alive。

### 后端轮询与故障恢复

- 每条 location 独立维护轮询位置，按配置顺序选择后端。
- 同一个后端地址被多条规则引用时，共享冷却状态。
- 连接失败、发送或接收故障、响应提前断开，以及后端交互超时，都会使对应节点进入 10 秒冷却。
- 冷却期间，新请求跳过该节点；冷却到期后，节点重新获得被选择的资格，由实际请求触发恢复尝试。
- 当前连接或传输故障请求返回 502，超时请求返回 504，后续请求选择其他可用节点。
- 后端返回格式有效且完整的 HTTP 4xx、5xx 响应时，正常转发响应，节点继续参与轮询。

轮询位置、冷却截止时间和后端连接状态统一由 I/O 线程维护。

冷却时间由 `include/tiny_http/linux_server.h` 中的 `backend_cooldown_ms_` 控制，默认值为 `10000` 毫秒。

配置中未启动的后端也会参与选择。请求访问该节点失败后进入冷却；冷却到期后再次被选中时，会重新尝试连接。

### 后端响应结束条件

| 响应类型 | 完成条件 |
| --- | --- |
| HEAD、204、304 | 响应头接收完成 |
| 带 Content-Length 的普通响应 | 正文达到声明的字节数 |
| 无 Content-Length 的普通响应 | 后端正常关闭发送方向，recv 返回 0 |

后端声明了 Content-Length，却在正文收齐前关闭连接时，代理返回 502。

后端通过关闭连接结束正文时，代理根据实际收到的正文字节数，为客户端生成 Content-Length。

当前请求解析拒绝 Transfer-Encoding；后端响应带有 Transfer-Encoding 时，代理返回 502。

### 线程职责

| 线程 | 职责 |
| --- | --- |
| 主 I/O 线程 | epoll 事件循环、客户端与后端 Socket、连接状态、网络收发和超时清理 |
| 工作线程池 | 静态文件请求处理，默认 4 个线程 |
| 日志线程 | 异步写入日志文件 |

Connection 和 Upstream 由 I/O 线程维护。工作线程通过 ResponseResult 交回结果，避免跨线程修改连接状态。

## 超时与资源限制

### 三类超时

| 类型 | 默认值 | 计时范围 | 超时处理 |
| --- | --- | --- | --- |
| 后端超时 | 15 秒 | 建立后端连接、发送请求、接收完整响应的累计时间 | 关闭后端连接，将对应后端冷却 10 秒，向客户端返回 504 |
| 发送超时 | 15 秒 | 向客户端发送时连续无进展的时间 | 关闭客户端连接并释放关联资源 |
| 空闲超时 | 15 秒 | 未处于请求处理状态的客户端连接无收发活动的时间 | 清理客户端连接 |

成功向客户端发送数据时更新发送进度时间；后端超时按整次交互累计计时。

发送超时由 `send_timeout` 配置，空闲超时由 `keep_alive` 配置。后端超时使用 `include/tiny_http/linux_server.h` 中的 `proxy_timeout_ms_` 常量。

超时检查在每轮事件循环结束时执行。

### 大小与队列限制

| 项目 | 默认限制 | 超限处理 |
| --- | --- | --- |
| 客户端请求头 | 8 KiB，包含请求行、头字段及结束空行 | 关闭客户端连接 |
| 客户端请求体 | 1 MiB，由 max_body_size 配置 | 关闭客户端连接 |
| 客户端输入缓冲区 | max_body_size + 8 KiB | 关闭客户端连接 |
| 工作线程池排队任务 | 256，由 max_pending_tasks 配置 | 返回 503 |
| 后端响应头 | 8 KiB，包含状态行、头字段及结束空行 | 返回 502 |
| 后端响应正文 | 8 MiB | 返回 502 |

后端响应限制使用 `proxy_header_limit_` 和 `proxy_body_limit_` 常量。

请求头中的重复 Content-Length、非法长度值以及超出请求体上限的声明，在请求解析阶段处理。后端响应解析同样严格检查 Content-Length。

### 资源释放

- 静态文件发送完成后释放文件描述符。
- 工作线程结果对应的客户端已经关闭时，释放结果携带的文件描述符。
- 代理完成或失败时，关闭后端 Socket 并移除对应 Upstream。
- 客户端连接关闭时，同时清理关联后端连接和静态文件描述符。
- 收到 SIGINT、SIGTERM 后退出事件循环，停止工作线程，回收剩余连接和文件资源，最后停止日志线程。

## 主要文件

| 路径 | 作用 |
| --- | --- |
| `src/main.cpp` | 程序入口、配置加载和信号设置 |
| `src/linux_server.cpp` | epoll 事件循环、客户端收发、静态请求处理和连接清理 |
| `src/http_proxy.cpp` | 后端轮询、故障冷却、后端建连、请求转发、响应解析和代理资源管理 |
| `src/http_parser.cpp` | HTTP 请求解析与长度校验 |
| `src/http_response.cpp` | HTTP 响应生成 |
| `src/config_parser.cpp` | 配置文件解析和命令行参数覆盖 |
| `src/thread_pool.cpp` | 工作线程池 |
| `src/async_logger.cpp` | 异步日志 |
| `src/mime_types.cpp` | 文件扩展名与 MIME 类型映射 |
| `include/tiny_http/` | HTTP 服务器相关声明和队列实现 |
| `tests/` | 模块测试 |
| `www/` | 静态资源目录 |
| `tiny_httpd.conf` | HTTP 服务器配置 |
| `CMakeLists.txt` | 构建配置 |

## 静态文件性能记录

以下为 sendfile + TCP_NODELAY 调优阶段保留的静态文件历史测量结果。

### 测量环境

| 项目 | 设置 |
| --- | --- |
| 系统环境 | VMware Ubuntu 虚拟机 |
| 可用逻辑 CPU | 4 |
| 工作线程数 | 4 |
| 压测工具 | wrk debian/4.1.0-4build2 |
| 压测线程 | 4 |
| 并发连接 | 100 |
| 单轮时长 | 30 秒 |
| 访问地址 | 127.0.0.1:8080 |

服务端和 wrk 位于同一虚拟机，使用回环网络。数据包含页缓存、回环传输和虚拟机调度的影响。

### 历史结果

表中记录为最初基线与一组完整优化后复测结果。

| 文件大小 | 版本 | Requests/sec | 平均延迟 | P99 |
| --- | --- | ---: | ---: | ---: |
| 4 KiB | 优化前 | 65,542.88 | 1.52 ms | 3.65 ms |
| 4 KiB | sendfile + TCP_NODELAY | 60,788.03 | 1.64 ms | 2.97 ms |
| 100 KiB | 优化前 | 24,467.31 | 3.99 ms | 10.02 ms |
| 100 KiB | sendfile + TCP_NODELAY | 35,012.17 | 2.78 ms | 6.59 ms |
| 1 MiB | 优化前 | 3,302.45 | 29.79 ms | 45.37 ms |
| 1 MiB | sendfile + TCP_NODELAY | 10,745.17 | 7.43 ms | 14.70 ms |

- 4 KiB 请求吞吐下降约 7.3%，P99 从 3.65 ms 降至 2.97 ms。
- 100 KiB 请求吞吐提升约 43.1%。
- 1 MiB 请求吞吐达到基线约 3.25 倍，平均延迟从 29.79 ms 降至 7.43 ms。

采用 sendfile 后，文件正文直接从文件描述符发送到客户端 Socket，省去将整个文件正文组装进用户态 HTTP 响应字符串的步骤。

发送调整为“send 响应头 + sendfile 正文”时，小文件场景曾出现约 41 ms 的平均延迟；开启 TCP_NODELAY 后，记录到的平均延迟恢复至约 1.6～1.8 ms。

### 压测命令示例

创建一个 1 MiB 静态文件：

```bash
dd if=/dev/zero of=www/bench_1m.bin bs=1M count=1
```

保持服务器运行，在另一个终端执行：

```bash
wrk -t4 -c100 -d30s --latency \
    http://127.0.0.1:8080/bench_1m.bin
```

## 模块测试

完成 Debug 构建后执行：

```bash
ctest --test-dir build --output-on-failure
```

现有测试目标：

| 目标 | 内容 |
| --- | --- |
| `parser_tests` | HTTP 请求解析、请求体解析、MIME 查询和响应序列化 |
| `config_parser_tests` | 配置语法、默认值和命令行参数覆盖 |
| `room_protocol_tests` | TCP 房间协议编解码及消息边界处理 |

## 附属 TCP 程序

仓库还包含独立的 `tiny_roomd`，用于 TCP 长连接、消息回显和房间广播练习。

```bash
./build/tiny_roomd
```

默认监听 `127.0.0.1:9090`，使用自定义二进制协议。实现位于 `src/room_server.cpp`，协议定义位于 `include/tiny_room/protocol.h`。