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
```

### 运行（开发配置）

```bash
./scripts/run_dev.sh
```

默认配置文件：`configs/dev/forwarder.json`

- **client**：`0.0.0.0:19000`
- **admin**：`127.0.0.1:19003`

## 测试脚本与使用方式

完整测试与工具说明见 **`docs/testing.md`**。常用入口：

```bash
pip install -r tools/requirements-relay.txt
export PYTHONPATH=tools

# 交互式控制台（推荐：一个终端管理多连接、发包、压测、拉取 admin）
python3 tools/relay_cli.py --host 127.0.0.1 --port 19000 --admin-port 19003

# 或一键拉起 build/asio_forwarder（随机端口）后进入控制台
python3 tools/relay_cli.py --spawn-server
```

自动化回归：

```bash
cd build && ctest --output-on-failure
# 或
./scripts/test_e2e.sh
```

## 最终呈现：Web 前端交互界面（本地）

仓库提供一个“浏览器界面”用于展示完整功能（注册/登录、DATA、CONTROL、KICK）。它通过一个轻量桥接服务把浏览器操作转换为 TCP 协议帧（中继仍是黑盒）。

```bash
pip install -r tools/requirements-relay.txt -r tools/requirements-webui.txt
export PYTHONPATH=tools
python3 tools/webui_server.py --relay-host 127.0.0.1 --relay-port 19000
```

然后打开 `http://127.0.0.1:8080`。

## 最终交付：接口（SDK）与真实场景示例

- **C++ SDK（接口封装）**：`include/fwd/relay_client.hpp` + `src/relay_client.cpp`（CMake target：`asio_forwarder_sdk`）  
- **真实场景示例（C++）**：`examples/realistic_scenario_cpp/`（`dispatcher_cpp` / `worker_cpp` / `admin_cpp`，按用户名路由并覆盖功能）  
- **Python**：仓库中的 Python 主要用于联调/自测/演示；生产交付可不包含 Python

## 某次压测数据（样例）

说明：以下为历史 **`relay_cli.py --spawn-server`** 在旧协议（Body `op` + 旧广播语义）下的一次 loopback 结果，**数值与机器/内核/网络栈/客户端相关**，仅作相对参考；**当前协议已改为 `msg_type` + 按目标用户广播/轮询**，若需对比请在现版本复跑。

| 场景 | payload | count | window | 发送速率（约） | ACK RTT p50/p95/p99 (ms) |
|------|--------:|------:|-------:|------------------------:|--------------------------:|
| unicast → 单接收者 | 32 B | 20,000 | 256 | ≈ 7,363 | ≈ 1 / 7 / 44 |
| unicast | 1,024 B | 10,000 | 128 | ≈ 6,700 | ≈ 2 / 10 / 15 |
| unicast | 65,536 B | 1,000 | 32 | ≈ 4,478 | ≈ 3 / 6 / 12 |
| 旧 broadcast（多其他用户各 1 条） | 32 B | 20,000 | 256 | ≈ 3,076 | ≈ 75 / 92 / 101 |
| 旧 round_robin | 32 B | 20,000 | 256 | ≈ 4,064 | ≈ 66 / 86 / 93 |

## 文档导航

- `docs/protocol.md`：协议与字段（v2 header、`msg_type`、200/201/202）
- `docs/client_api.md`：Python `RelayClient` 说明
- `docs/delivery.md`：**交付清单（Server/Client/SDK/示例/WebUI）**
- `docs/architecture.md`：组件与线程/配置注意点
- `docs/data_flow.md`：读帧 → 路由 → 写出数据流
- `docs/design.md`：取舍与扩展方向
- `docs/testing.md`：测试脚本、交互控制台、压测说明
- `docs/history.md`：**版本更迭/历史变化（与旧版差异）**

## 重要实现提醒（与直觉可能不同）

- **CONTROL 权限**：仅 **`peer_role == admin`** 的账号可 `list_users` / `kick_user`（见 `docs/protocol.md`）。
- **Header.flags**：位定义存在，但当前主路由不依赖 flags。
- **账户数据**：进程内存存储，**重启即丢失**。
