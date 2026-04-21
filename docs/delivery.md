# 交付清单（服务器端 + 客户端端）

本文用于“最终交付”时明确：**服务器端需要交付什么**、**客户端侧（业务方）需要拿到什么**、以及推荐的目录/运行方式。

> 约定：`asio_forwarder` 作为黑盒中间件运行在服务器；业务方在自己的机器/进程中使用 C++ SDK 接入；本仓库也提供一个 Web 前端（Python 桥接）用于本地交互演示。

## 服务器端交付（Server）

### 必交付

- **可执行文件**：`build/asio_forwarder`
- **配置文件模板**：`configs/dev/forwarder.json`（交付时通常会提供环境化配置，如端口/超时/限流参数等）
- **运行脚本（可选但推荐）**：`scripts/run_dev.sh`（或你们公司的服务启动脚本/容器入口）
- **协议说明**：`docs/protocol.md`

### 运行方式（示例）

```bash
./build/asio_forwarder configs/dev/forwarder.json
```

其中：
- **业务 TCP 端口**：`client.listen.port`（所有客户端连这一个端口）
- **admin HTTP 端口**：`admin.listen.port`（health/stats/events/users）

## 客户端端交付（Client / SDK）

### 必交付：C++ SDK

- **头文件**：`include/fwd/relay_client.hpp`
- **实现**：`src/relay_client.cpp`
- **CMake target**：`asio_forwarder_sdk`（静态库 `libasio_forwarder_sdk.a`）

业务方接入时只需要依赖 SDK（不需要理解服务端实现），典型流程：
- `connect(host, port)`
- `login(username, password, peer_role, register)`
- `send_unicast/broadcast/round_robin(dst_username, payload, ...)`
- （推荐）`send_*_typed(dst_username, type, obj, ...)`：把业务 payload 统一封装为 msgpack `{type,data}`（详见 `docs/protocol.md`）
- `recv()` 收 `Deliver/ServerReply/Kick`

### 必交付：C++ 真实业务模拟（示例工程）

目录：`examples/realistic_scenario_cpp/`

包含三类可执行程序（示例，业务方可按需裁剪）：
- `build/dispatcher_cpp`
- `build/worker_cpp`
- `build/admin_cpp`

用途：
- 展示 **注册/登录、unicast、broadcast、round_robin、CONTROL、KICK** 的完整端到端流程
- 展示“同一用户名多连接”的真实情况（同一用户开多个进程连接）
- 展示“业务 payload 推荐结构 `{type,data}`”与 C++ `struct` 的序列化/反序列化（`MSGPACK_DEFINE`）

运行步骤见：`examples/realistic_scenario_cpp/README.md`

## 本地交互演示（可选交付 / 推荐研发保留）

### Web 前端交互界面（唯一前端）

用于本地测试/演示全部功能（注册/登录、DATA、CONTROL、KICK）：

- **桥接服务**：`tools/webui_server.py`
- **前端页面**：`tools/webui_static/index.html`
- **依赖**：`tools/requirements-webui.txt`、`tools/requirements-relay.txt`

启动：

```bash
pip install -r tools/requirements-relay.txt -r tools/requirements-webui.txt
export PYTHONPATH=tools
python3 tools/webui_server.py --relay-host 127.0.0.1 --relay-port 19000
```

打开：`http://127.0.0.1:8080`

> 说明：WebUI 是“演示/联调工具”，不影响 C++ 交付主链路；生产交付可以不包含 Python。

## 交付时建议打包的最小集合（推荐）

- Server:
  - `build/asio_forwarder`
  - 一份环境配置（基于 `configs/dev/forwarder.json` 改）
  - `docs/protocol.md`
- Client:
  - `include/fwd/relay_client.hpp`
  - `src/relay_client.cpp` 或 `libasio_forwarder_sdk.a`
  - `examples/realistic_scenario_cpp/`（含 README + 三个可执行示例）

