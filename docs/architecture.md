# 架构说明（asio-forwarder / 对等中继）

## 当前实现（C++，`src/main.cpp`）

- **单业务端口** `client.listen`（JSON 亦兼容旧键 `upstream.listen`）：所有客户端对等接入，**无上游/下游之分**。
- 每连接协程 **`TcpSession::read_loop`**：读 **40B v2 头** → 校验 magic/version/header_len → 读 `body_len` → **`RelayServer::handle_client_frame`**。
- Body 为 **msgpack map**，按 **`op`**：**LOGIN**（首包）→ **HEARTBEAT / TRANSFER / CONTROL**。
- **用户表**：`user_id → deque<TcpSession>`；多连接、超限踢最老、同用户新 `control` 替换旧 `control`。
- **TRANSFER**：
  - **unicast**：`pick_preferred(dst)` → 发 **200**；
  - **broadcast**：除发送方外在线用户各一条（顺序为 `std::set` 用户 id），扇出上限 **`session.broadcast_max_recipients`**；
  - **round_robin / roundrobin**：异步定时逐用户发 **200**。
- **背压**：`TcpSession::send_frame`，每连接队列 + **`flow.send_queue.*`**（高水位 drop 或 disconnect，硬上限断连）。
- **admin HTTP**（Boost.Beast）：仅 **GET**；`/` 未列出的路径返回 404。
  - `/api/health`、`/health` → `{"ok":true}`
  - `/api/stats` → JSON（sessions、users、frames、bytes、drops、pending_bytes 等）
  - `/api/events` → JSON 事件数组（容量 `admin.events_max`）
- **定时器**：`tick_metrics`（日志）、`tick_heartbeat`（已登录会话超时踢线）。
- **依赖**：Boost.Asio/Beast；**msgpack-cxx**（CMake FetchContent，`MSGPACK_NO_BOOST`）。

## 线程与配置注意

- **`threads.io`**：`main` 中创建多个 `std::thread` 调用 `io.run()`，共用一个 `io_context`。
- **`threads.biz`**：`Config` 可从 JSON 读取，**当前工程未使用其启动独立业务线程池**（与实现不一致时以代码为准）。

## Python 工具链（非 Web 自测 / 压测）

```bash
pip install -r tools/requirements-relay.txt
export PYTHONPATH=tools
```

| 脚本 | 作用 |
|------|------|
| `relay_proto.py` | v2 头 + msgpack 组帧/收包，与 C++ 一致 |
| `relay_e2e_runner.py` | 最短 e2e：LOGIN + TRANSFER unicast + DELIVER 校验 |
| `relay_test_suite.py` | 多场景：broadcast、round_robin、CONTROL、非法包断连等 |
| `relay_cli.py` | **交互式**多连接、`send`/`flood`、`admin` 拉取、可选 `--spawn-server` |

自动化：

```bash
cd build && ctest --output-on-failure
./scripts/test_e2e.sh
```

详见 **[`docs/testing.md`](testing.md)**。

## 相关文档

- [`docs/protocol.md`](protocol.md)：线格式与 msgpack 字段
- [`docs/design.md`](design.md)：设计取舍
- [`docs/data_flow.md`](data_flow.md)：读帧与转发路径
- [`docs/testing.md`](testing.md)：测试与压测
