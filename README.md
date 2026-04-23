# asio-forwarder（对等 TCP 中继）

`asio-forwarder` 是一个 **C++20 + Boost.Asio（协程）** 实现的 **对等（peer-to-peer）TCP 中继服务**：所有客户端连接同一业务端口，首帧 **`msg_type=1`（login）** 完成注册或登录后，通过 **`msg_type=2/3/4`**（心跳、控制、数据）与服务器交互。

协议：**v2 线格式（40B 小端 header）+ msgpack body**；服务器下行主要有：**200 DELIVER**、**201 SERVER_REPLY**、**202 KICK**。权威说明见 **`deliver/docs/protocol.md`**。

## 仓库结构（交付 vs 本地）

| 目录 | 含义 |
|------|------|
| **`deliver/`** | **交付包**源码树：`server/`（`main.cpp`、配置模板）、`client/`（`include/fwd/` + SDK 源）、`docs/`。详见 **`deliver/README.md`**。 |
| **`local/`** | **本地**测试与联调：`tests/`、`tools/`、`examples/`；**依赖** `deliver/` 与 `build/`。详见 **`local/README.md`**。 |
| **`build/`** | CMake 生成物（`asio_forwarder`、`libasio_forwarder_sdk.a`、`forwarder_cpp_smoke` 等）。 |
| **`scripts/`** | `build.sh`、`run_dev.sh`、与 `local/tests/run_e2e.sh` 的兼容入口。 |

## 项目特征

- **单端口对等接入**：`client.listen` 一处配置。
- **用户与多连接**：每用户最多 **`session.max_connections_per_user`** 条 TCP（默认 8）。
- **透明载荷**：不解析 **DATA** 的 `payload` 业务语义。
- **稳定性保护**：限长、超时、心跳踢线、发送队列背压；踢线前尽量发 **202**。

## 架构（简要）

- 进程入口：`deliver/server/src/main.cpp`（与 SDK **共用** `deliver/client/include/fwd/*` 协议头）
- 关键类型：**`RelayServer`**、**`TcpSession`**
- 更多见 **`deliver/docs/architecture.md`**

## 使用

### 构建与回归

```bash
./scripts/build.sh
cd build && ctest --output-on-failure
```

### 开发运行

```bash
./scripts/run_dev.sh
```

默认读 **`deliver/server/forwarder.json`**。端口以该文件为准（常见为 `0.0.0.0:19000` + `127.0.0.1:19003` admin）。

## 文档导航

| 主题 | 位置 |
|------|------|
| 线协议 | `deliver/docs/protocol.md` |
| 交付物清单 | `deliver/docs/delivery.md` |
| 本地测试与联调 | `deliver/docs/local.md`（`deliver/docs/testing.md` 仅重定向至本页） |
| 客户端 API | `deliver/docs/client_api.md` |
| 架构 / 数据流 / 设计 | `deliver/docs/architecture.md` 等 |
| 与旧版差异 | `deliver/docs/history.md` |

## 重要提醒

- **CONTROL** 仅 **`peer_role == admin`** 可用（见协议文档）。
- **账户**在内存，**进程重启即丢失**。
