# 交付清单（服务器端 + 客户端端）

**构建、自测、WebUI 启动、历史压测表**：见 `README.md` 与 `docs/testing.md`（本文不重复「怎么跑起来」的指令）。

**自动化状态**：`ctest`（Python e2e+套件）与 **C++ smoke** 的分工、以及可选的**回归记录表**，见 **`docs/testing.md` 的「测试分层」「回归验证记录」**；Web 桥**无**机器用例，仍属人工验收范围。

目的：列清 **服务器** / **C++ 集成方** 各自应拿哪些产物；**Python 工具** 为联调/演示，可选。

> `asio_forwarder` 为黑盒中间件；业务进程用 C++ SDK 接入；Web 页面经 Python 桥接连 TCP 中继，仅作本地演示。

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

- **对外主用 API（业务集成方）**：`include/fwd/asio_forwarder_client.hpp` + `src/asio_forwarder_client.cpp`（ConnectionConfig、Client：open / sign_on / heartbeat / send / recv_deliver、管理员 CONTROL 等；与 Web 桥用法一致：连上 → 身份 → 发收）
- **线协议层（随库提供，一般不必直接 #include）**：`include/fwd/relay_client.hpp` + `src/relay_client.cpp`（由 asio_forwarder_client 在内部使用）
- **CMake target**：`asio_forwarder_sdk`（静态库 `libasio_forwarder_sdk.a`，包含上述所有 .cpp）

**推荐**走 `asio_forwarder_client`：`open(ConnectionConfig)` → `sign_on(..., register_new)` → `send(SendMode, ...)` / `send_typed` / `send_poly` → `recv_deliver()`，管理员另用 `control_list_users` / `control_kick_user`。需帧级控制时再使用 `Client::raw()` 取 `RelayClient`。

### 必交付：C++ 真实业务模拟（示例工程）

目录：`examples/realistic_scenario_cpp/`

包含三类可执行程序（示例，业务方可按需裁剪）：
- `build/dispatcher_cpp`
- `build/worker_cpp`
- `build/admin_cpp`

用途：
- 展示 **注册/登录、unicast、broadcast、round_robin、CONTROL、KICK** 的完整端到端流程
- 展示“同一用户名多连接”的真实情况（同一用户开多个进程连接）
- 展示“业务 payload 可自定义为任意二进制 bytes”，以及一种可落地的“多态信封”结构 `{"kind","type","data"}`：
  - `kind` 用于接收端按枚举分发反序列化
  - `type` 便于日志/前端展示
  - `data` 为业务对象（C++ 侧用 `MSGPACK_DEFINE`）

运行步骤见：`examples/realistic_scenario_cpp/README.md`

## 本地交互演示（可选 / 建议仅研发保留）

- 组件：`tools/webui_server.py` + `tools/webui_static/index.html`；依赖见 `tools/requirements-*.txt`。
- **安装与启动命令**见 `docs/testing.md` 的「Web 前端」一节；默认 `http://127.0.0.1:8080`。
- 生产交付**可不包含** Python；Web 仅作联调与多态 payload 演示。

## 交付时建议打包的最小集合（推荐）

- Server:
  - `build/asio_forwarder`
  - 一份环境配置（基于 `configs/dev/forwarder.json` 改）
  - `docs/protocol.md`
- Client:
  - `include/fwd/relay_client.hpp`
  - `src/relay_client.cpp` 或 `libasio_forwarder_sdk.a`
  - `examples/realistic_scenario_cpp/`（含 README + 三个可执行示例）

