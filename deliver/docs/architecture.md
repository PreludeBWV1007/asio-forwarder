# 架构说明（asio-forwarder / 对等中继）

## 当前实现（C++，`deliver/server/src/main.cpp`）

- **单业务端口** `client.listen`（JSON 亦兼容旧键 `upstream.listen`）：所有客户端对等接入，**无上游/下游之分**。
- 每连接协程 **`TcpSession::read_loop`**：读 **40B v2 头** → 校验 magic/version/header_len → 读 `body_len` → **`RelayServer::handle_client_frame`**。
- Body 为 **msgpack map**，由 **`Header.msg_type`** 区分：
  - **未登录**：仅 **`kClientLogin(1)`**；
  - **已登录**：**`kClientHeartbeat(2)`** / **`kClientControl(3)`** / **`kClientData(4)`**。
- **账户表**：`username → AccountRecord`（盐、口令摘要、`is_admin`、`user_id`）；**`uid_to_username_`** 反查展示名。
- **用户连接表**：`user_id → deque<TcpSession>`；多连接、超限踢最旧并 **202 KICK** 说明原因。
- **DATA（转发）**：
  - **unicast**：按 **`dst_username`** 定位目标用户，再 `find_user_connection(user_id, dst_conn_id)` → 发 **200**（msgpack 信封：`payload` + `src_conn_id` + `dst_conn_id` + `src_username` + `dst_username`；同时约定下行 header 的 src/dst user_id 置 0）；
  - **broadcast**：必须带 **`dst_username`**，向该用户**全部在线连接**扇出 **200**（上限 **`session.broadcast_max_recipients`**）；
  - **round_robin**：必须带 **`dst_username`**，`co_spawn` + `steady_timer` 对该用户各连接**等间隔**发 **200**。
- **背压**：`TcpSession::send_frame`，每连接队列 + **`flow.send_queue.*`**（高水位 drop 或 disconnect，硬上限断连）。
- **admin HTTP**（Boost.Beast）：仅 **GET**；`/` 未列出的路径返回 404。
  - `/api/health`、`/health` → `{"ok":true}`
  - `/api/stats` → JSON（sessions、users、frames、bytes、drops、pending_bytes 等）
  - `/api/events` → JSON 事件数组（容量 `admin.events_max`）
  - **`/api/users`** → JSON：用户列表及每条连接的 `conn_id`、`endpoint`
- **定时器**：`tick_metrics`（日志）、`tick_heartbeat`（已登录会话超时 → **202** + 断开）。
- **依赖**：Boost.Asio/Beast；**msgpack-cxx**（CMake FetchContent，`MSGPACK_NO_BOOST`）；口令摘要见 **`deliver/client/include/fwd/sha256.hpp`**。

## 线程与配置注意

- **`threads.io`**：`main` 中创建多个 `std::thread` 调用 `io.run()`，共用一个 `io_context`。
- **`threads.biz`**：`Config` 可从 JSON 读取，**当前工程未使用其启动独立业务线程池**（与实现不一致时以代码为准）。

## Python 工具链（非 Web 自测 / 压测）

```bash
pip install -r local/tools/requirements-relay.txt
export PYTHONPATH=tools
```

| 脚本 | 作用 |
|------|------|
| `local/tools/forwarder_wire.py` | v2 头 + msgpack 组帧/收包、常量（与 `local/tests/`、CLI 共用） |
| `local/tests/e2e_minimal.py` | 最短 e2e：注册/登录 + HEARTBEAT + DATA 单播 + DELIVER 校验 |
| `local/tests/e2e_suite.py` | 多场景：广播到用户全连接、按用户轮询、CONTROL+KICK、非法包断连等 |
| `relay_cli.py` | **交互式**多连接、`send`/`flood`、`admin`（含 `users`）、可选 `--spawn-server` |
| `run_scenario.py` | 较长联调脚本（注册多用户 + DATA + CONTROL） |

自动化：

```bash
cd build && ctest --output-on-failure
./local/tests/run_e2e.sh
```

详见 **[local.md](local.md)**。

## 相关文档

- [protocol.md](protocol.md)：线格式与 msgpack 字段
- [client_api.md](client_api.md)：Python 客户端接口
- [design.md](design.md)：设计取舍
- [data_flow.md](data_flow.md)：读帧与转发路径
- [local.md](local.md)：本地测试与联调
