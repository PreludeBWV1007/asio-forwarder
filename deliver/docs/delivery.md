# 交付说明

> **源码树**：`deliver/`。构建产物在仓库根 `build/`。  
> **`local/`**（回归脚本、网页、样例）**不在**客户最小交付包内；开发者自参见仓库 [local/README.md](../../local/README.md)。

---

## 1. 目录与产物

| 路径 | 说明 |
|------|------|
| `deliver/server/src/main.cpp` | **唯一服务端源文件**（含中继逻辑与 MySQL 访问实现） |
| `deliver/server/include/mysql_store.hpp` | MySQL 封装类声明 |
| `deliver/server/forwarder.json` | 配置模板 |
| `deliver/server/schema.sql` | 库表：`ip_allowlist`、`users` |
| `deliver/client/include/fwd/` | **对外头文件**（协议、配置、**forwarder_client.hpp** 等） |
| `deliver/client/src/forwarder_sdk.cpp` | **唯一客户端库源文件**（同步 Relay + 高层 Client） |
| `build/asio_forwarder` | 服务端可执行文件 |
| `build/libasio_forwarder_sdk.a` | 客户端静态库 |

---

## 2. 配置文件主要键（JSON）

| 键 | 含义 |
|----|------|
| `client.listen` | 业务 TCP：`host` / `port` / `backlog` |
| `admin.listen` | 只读 HTTP：`host` / `port`（常见本机） |
| `mysql` | `host` `port` `user` `password` `database` |
| `threads.io` | `io_context` 线程数 |
| `timeouts` | `read_ms` / `idle_ms` |
| `limits.max_body_len` | 单帧正文最大长度 |
| `flow.send_queue` | 发送队列高水位与策略 `drop` \| `disconnect` |
| `session` | `max_connections_per_user`、`heartbeat_timeout_ms`、`broadcast_max_recipients` |

完整定义见 `deliver/client/include/fwd/config.hpp`。

---

## 3. 数据库表与职责

| 表 | 作用 |
|----|------|
| `ip_allowlist` | 允许发起 TCP 的**源 IP**（字符串全匹配，常含 `127.0.0.1`） |
| `users` | `username` / `password`（当前为明文，生产请改摘要） / `is_admin` |

登录：**先**白名单 **再** 账号；若用户名不存在则**自动插入**后登录（仍须过白名单）。

---

## 4. 客户端 API（C++，集成方）

**包含**：`#include "fwd/forwarder_client.hpp"`  
**链接**：`libasio_forwarder_sdk.a`，并链接 `pthread`、`boost_system`、`msgpack-cxx`（与 CMake target `asio_forwarder_sdk` 一致）。

命名空间 **`fwd::asio_forwarder_client`**：

| 类型 / 函数 | 作用 |
|-------------|------|
| `Client` | 一条 TCP：**open** → **sign_on**（阻塞直到 LOGIN 成功或异常）→ **send** / **recv_deliver** / **heartbeat** / **control_*** |
| `Client::ConnectionConfig` | `host` + `port` |
| `RecvMode` | `Broadcast` / `RoundRobin`（与登录时字符串 `broadcast` / `round_robin` 对应） |
| `Client::send(target_username, payload_bytes, SendOptions)` | 发 DATA；可选等待 201 受理 |
| `Client::recv_deliver()` | 阻塞收一条 200 投递 |
| `Client::control_list_users` / `control_kick_user` | 管理命令（需管理员账号） |
| `try_login(...)` | 仅探测登录是否成功 |
| `LocalForwarder` | **Linux 联调用**：fork 子进程跑 `asio_forwarder` 临时配置（可选） |
| `admin_health_ok(host, admin_port)` | GET `/api/health` 探测 |

底层同步套接字实现类 **`fwd::sdk::RelayClient`**（头文件 `relay_client.hpp`）：`connect` / `login` / `send_data` / `recv` 等；一般业务优先用上层 `Client`。

---

## 5. 只读管理口 HTTP

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health`、`/health` | 存活 |
| GET | `/api/stats` | 连接数、帧/字节计数等 |
| GET | `/api/users` | 在线用户、`recv_mode`、连接编号与对端地址、`is_admin` |
| GET | `/api/events` | 最近事件环形缓冲 |
| OPTIONS | `*` | CORS 预检 |

---

## 6. 可选交付物

- **`local/tools/`**：Python 线缆工具、**Web 单页**（监控 + 模拟终端）、`e2e_forwarder.py` 等。  
- **样例**：`local/examples/first_use/`（注释入门 C++）；性能小工具见 `local/tests/perf_basic.cpp`（`forwarder_perf`）。

---

## 7. 协议与性能

- 线协议全文：**[protocol.md](protocol.md)**  
- 性能测试与填表：**[performance.md](performance.md)**

---

## 8. 能力与操作一览（拿到软件后你能「做」什么）

**可以同时用**：本机浏览器打开 `local/tools/webui_server.py` 提供的页面做联调，业务代码用 C++ SDK 连**同一业务端口**——二者互不排斥，只要 MySQL 里账号与白名单允许你的来源 IP。

### 8.1 服务端进程（`asio_forwarder` + 配置文件）

| 能力 | 说明 |
|------|------|
| 监听业务 TCP | `client.listen`：客户端 LOGIN / HEARTBEAT / DATA / CONTROL 均走此端口 |
| 鉴权 | 先查 `ip_allowlist`，再查 `users`；新用户名可自动插入后登录（仍须过白名单） |
| 按登录名路由 DATA | 不解析业务语义，只按**目标用户名**与目标当前 `RecvMode` 投递 |
| 只读 HTTP 管理口 | `admin.listen`：`/api/health`、`/api/stats`、`/api/users`、`/api/events` 等（见第 5 节） |
| 踢线、管理 CONTROL | 由**管理员账号**在业务 TCP 上发 CONTROL；观测结果也可在 `/api/users` 看到 |

服务端**不提供**在二进制协议里「列出可执行函数表」——可操作项即：各 JSON 配置项 + 上述 HTTP 路径 + 线协议规定的帧类型（见 protocol.md）。

### 8.2 C++ 客户端库（`libasio_forwarder_sdk.a` + `fwd/forwarder_client.hpp`）

| API / 对象 | 典型用途 |
|------------|----------|
| `Client::open` | 建立到中继的 TCP |
| `Client::sign_on` | LOGIN，选 `RecvMode`（Broadcast / RoundRobin）与 `peer_role` |
| `Client::local_username` | 确认当前登录名 |
| `Client::send` | 向某**登录名**发二进制 DATA；`SendOptions::wait_server_accept` 是否等待 201 |
| `Client::send_poly` / `send_typed` | 在载荷外再包 msgpack（kind/type/data 或 type/data），仍走 DATA 路由 |
| `Client::recv_deliver` | 阻塞收一条发往本连接的投递（200） |
| `Client::heartbeat` | 发 HEARTBEAT，配合服务端超时配置 |
| `Client::control_list_users` / `control_kick_user` | 管理员：列出用户、按 user id 踢 |
| `Client::raw` | 访问 `sdk::RelayClient`：**connect/login/send_data/recv** 等更低层 |
| `try_login` | 短连接探测账号是否可登录 |
| `admin_health_ok` | HTTP GET 探测管理口 |
| `LocalForwarder` | （Linux）fork 子进程起临时中继，多用于自测 |

带逐步中文注释的**最小闭环示例**（单进程双连接）：仓库内 **`local/examples/first_use/first_use_client.cpp`**，构建产物 **`first_use_client`**。

### 8.3 与 `perf_basic.cpp` 的关系

`local/tests/perf_basic.cpp` 侧重**测往返耗时**（`perf_src` / `perf_dst`、循环 RTT）。  
**`first_use_client`** 侧重**可读性与注释**，使用常见 e2e 种子用户 `e2e_alice` / `e2e_bob`，适合第一次照抄改参数集成。
