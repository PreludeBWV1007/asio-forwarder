# asio-forwarder（对等 TCP 中继）

`asio-forwarder` 是一个 **C++20 + Boost.Asio（协程）** 实现的 **对等（peer-to-peer）TCP 中继服务**：所有客户端连接同一业务端口，首帧 **`msg_type=1`（login）** 完成注册或登录后，通过 **`msg_type=2/3/4`**（心跳、控制、数据）与服务器交互。

协议：**v2 线格式（40B 小端 header）+ msgpack body**；服务器下行主要有：**200 DELIVER**（msgpack：`payload` + 连接元数据）、**201 SERVER_REPLY**、**202 KICK**（断开原因）。详见 **`docs/protocol.md`**。

## 项目特征

- **单端口对等接入**：配置简单（仅 `client.listen`），无“上游/下游”固定角色。
- **用户与多连接**：内存账户（用户名 + 口令摘要 + `user_id`）；每用户最多 **`session.max_connections_per_user`** 条 TCP（默认 8）；服务端维护 **`user_id → 连接 deque`**。
- **透明载荷**：服务器不解析 **DATA** 里 `payload` 的业务语义；仅封装 `src_conn_id` / `dst_conn_id` 等投递元数据。
- **稳定性保护**：最大 body 长度、读/空闲超时、心跳踢线、每连接发送队列背压（drop/disconnect）；踢线前尽量下发 **202** 说明原因。
- **可观测性**：admin 只读 HTTP：`/api/health`、`/api/stats`、`/api/events`、**`/api/users`**（用户与连接快照）。
- **自测/压测工具齐全**：e2e、测试套件、交互式多连接控制台（支持 `flood`）。

## 架构与实现方式（简要）

- 进程入口：`src/main.cpp`
- 关键组件：
  - **`RelayServer`**：accept client/admin；维护账户表、`user_id → deque<session>`；**DATA** 路由（单播 / 按目标用户全连接广播 / 同用户连接轮询）；**CONTROL**（仅管理员）
  - **`TcpSession`**：协程读帧 + strand 串行写出；队列背压；**KICK** 先发后断
- 线程模型：`threads.io` 个线程跑同一个 `io_context`（配置中 `threads.biz` 当前未接入独立线程池）。
- 管理接口：Boost.Beast 处理 HTTP GET，返回 JSON。

更完整的架构说明见 `docs/architecture.md`、`docs/data_flow.md`、`docs/design.md`。

## 使用方法

### 构建

```bash
./scripts/build.sh
cd build && ctest --output-on-failure
```

`ctest`、**与 smoke 的分工**、**最近一次回归记录（日期与耗时）**见 **`docs/testing.md`**（`relay_cli` / e2e 亦在同文）。

### 运行（开发配置）

```bash
./scripts/run_dev.sh
```

默认配置文件：`configs/dev/forwarder.json`

- **client**：`0.0.0.0:19000`
- **admin**：`127.0.0.1:19003`

## 测试、Web 演示、SDK 与示例

- **自动测试、relay_cli、WebUI、`flood` 样例数**：**`docs/testing.md`**
- **生产交付物清单**（服务 / C++ SDK / 示例 / 可选 Web）：**`docs/delivery.md`**
- **C++ 对外 API 摘要**：**`docs/client_api.md`**（Python 见同文）

## 接口（SDK）与真实场景示例

- **C++ 主用 API**：`include/fwd/asio_forwarder_client.hpp` + `src/asio_forwarder_client.cpp`（同属 `asio_forwarder_sdk`；`relay_client` 在库内作为实现层）  
- **自测程序**：`examples/asio_forwarder_client_smoke/smoke.cpp` → `build/asio_forwarder_client_smoke`（`ctest` 中 `--connect` 连接随机口）
- **真实场景示例（C++）**：`examples/realistic_scenario_cpp/`（`dispatcher_cpp` / `worker_cpp` / `admin_cpp`，按用户名路由并覆盖功能；与上并列，按需保留）  
- **Python**：主要用于联调/自测/演示；纯 C++ 交付可不包含

## 文档导航

| 主题 | 文档 |
|------|------|
| 线协议（header、`msg_type`、200/201/202） | `docs/protocol.md` |
| 测试、`relay_cli`、**历史压测样例表**、WebUI 启动 | `docs/testing.md` |
| 交付物清单 | `docs/delivery.md` |
| 客户端（Python + C++ 入口） | `docs/client_api.md` |
| 架构 / 数据流 / 设计取舍 | `docs/architecture.md`、`docs/data_flow.md`、`docs/design.md` |
| 与旧版差异 | `docs/history.md` |

## 重要实现提醒（与直觉可能不同）

- **CONTROL 权限**：仅 **`peer_role == admin`** 的账号可 `list_users` / `kick_user`（见 `docs/protocol.md`）。
- **Header.flags**：位定义存在，但当前主路由不依赖 flags。
- **账户数据**：进程内存存储，**重启即丢失**。
