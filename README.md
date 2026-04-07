# asio-forwarder（对等 TCP 中继 + Msgpack 状态机）

**C++20 + Boost.Asio（协程）**。所有客户端连接**同一业务端口**，无「上游/下游」之分：先 **LOGIN** 绑定 `user_id`，再发 **HEARTBEAT / TRANSFER / CONTROL**。线格式为 **v2 头 40B + Body**；客户端上行 Body 为 **msgpack map**（详见 [`docs/protocol.md`](docs/protocol.md)）。

- **稳定性**：帧长上限、读/空闲超时、每连接发送队列背压、`session.heartbeat_timeout_ms` 心跳踢线。
- **依赖**：Boost；构建时 **FetchContent 拉取 msgpack-c**（`MSGPACK_NO_BOOST`）。

## 快速开始

```bash
cd asio-forwarder
./scripts/build.sh
./scripts/run_dev.sh
```

默认 [`configs/dev/forwarder.json`](configs/dev/forwarder.json)：

- **client**：`0.0.0.0:19000`（所有客户端连此口；配置键为 `client.listen`，兼容旧键 `upstream.listen`）
- **admin**：`127.0.0.1:19003`（`GET`：`/api/health`、`/health`、`/api/stats`、`/api/events`）
- **session**：每用户最大连接数、心跳、广播上限、轮流默认间隔等

## 测试与压测（汇总）

完整说明见 **[`docs/testing.md`](docs/testing.md)**，包括：

- CTest / `scripts/test_e2e.sh`
- `relay_e2e_runner.py`、`relay_test_suite.py`
- **交互式控制台** [`tools/relay_cli.py`](tools/relay_cli.py)（多连接、`flood`、admin 拉取）
- 一次 loopback 压测样例数据表

极简命令：

```bash
pip install -r tools/requirements-relay.txt
export PYTHONPATH=tools
python3 tools/relay_cli.py --spawn-server   # 或指定 --host/--port/--admin-port 连已有服务
```

自动化回归：

```bash
cd build && ctest --output-on-failure
# 或
./scripts/test_e2e.sh
```

`test_e2e.sh`：随机端口临时配置 → 启动 `asio_forwarder` → `relay_e2e_runner` + `relay_test_suite` → admin `/api/health`。

## Web 可视化

本仓库已移除旧 Protobuf / Web sidecar。如需可视化，需按当前 msgpack 协议单独实现。

## 协议与设计文档

| 文档 | 内容 |
|------|------|
| [`docs/protocol.md`](docs/protocol.md) | v2 帧、msgpack 字段、200/201、`msg_type` |
| [`docs/architecture.md`](docs/architecture.md) | 组件、线程与配置注意点 |
| [`docs/design.md`](docs/design.md) | 目标、取舍、扩展方向 |
| [`docs/data_flow.md`](docs/data_flow.md) | 读帧到 TRANSFER 投递路径 |
| [`docs/testing.md`](docs/testing.md) | 测试脚本、交互 CLI、压测样例 |

## 代码入口

- `src/main.cpp`：`RelayServer`、`TcpSession`、msgpack 处理、admin HTTP
- `include/fwd/protocol.hpp`、`include/fwd/relay_constants.hpp`、`include/fwd/config.hpp`、`include/fwd/session_policy.hpp`
- `tools/relay_proto.py`、`tools/relay_e2e_runner.py`、`tools/relay_test_suite.py`、`tools/relay_cli.py`

## 文档与实现对照（自检清单）

以下条目以 **`src/main.cpp` 与头文件为准**；若文档与代码冲突，以代码为准。

- 单端口对等、`op` 状态机、200 DELIVER / 201 SERVER_REPLY：[`docs/protocol.md`](docs/protocol.md) ↔ `handle_client_frame`、`relay_constants.hpp`
- **CONTROL**：当前实现**不校验** `role=="control"`，任意已登录连接可 `list_users` / `kick_user`（见 [`docs/protocol.md`](docs/protocol.md) 注记）
- **Header.flags**：位定义在 `protocol.hpp`，转发主路径以 Body 为主，未按 flags 路由
- **threads.biz**：JSON 可读入 `Config`，**当前进程仅使用 `threads.io` 跑 `io_context`**
- 配置键与默认值：`config.hpp` + `load_config_or_throw` + `configs/dev/forwarder.json`

## 常见问题

- **端口占用**：修改 `configs/dev/forwarder.json` 中 `client.listen.port` / `admin.listen.port`。
- **ctest 需网络**：首次 CMake 会下载 msgpack-c 源码包。
- **Python 缺 msgpack**：`pip install -r tools/requirements-relay.txt`。
