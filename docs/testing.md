# 测试与压测说明

本文描述如何运行自动化测试、Python 工具与交互式控制台，并记录一次在本仓库环境中测得的压测数据（**旧协议形态**）。**压测数值随 CPU、内核、是否 loopback、客户端实现语言而变**，仅作相对参考；以你本机复现为准。

## 依赖

```bash
pip install -r tools/requirements-relay.txt
```

需要 **Python 3** 与 **msgpack** 包。

## 构建与 CTest

```bash
./scripts/build.sh
cd build && ctest --output-on-failure
```

或：

```bash
./scripts/test_e2e.sh
```

`scripts/test_e2e.sh` 会：选取随机空闲端口写临时配置 → 启动 `build/asio_forwarder` → 运行 `tools/relay_e2e_runner.py` → 运行 `tools/relay_test_suite.py` → 请求 admin `GET /api/health`。

## 非交互自测脚本

在项目根目录：

```bash
export PYTHONPATH=tools

# 最小 e2e：注册/登录 + HEARTBEAT + DATA 单播 + 校验 DELIVER（含 conn 元数据）
python3 tools/relay_e2e_runner.py 127.0.0.1 19000

# 套件：unicast / 按用户广播全连接 / 按用户轮询 / CONTROL+KICK / 非法 msgpack 断连等
python3 tools/relay_test_suite.py 127.0.0.1 19000

# 较长联调（多用户注册 + DATA + 管理员踢人）
python3 tools/run_scenario.py 127.0.0.1 19000
```

端口与 `configs/dev/forwarder.json` 中 `client.listen.port` 一致；若改配置请同步改命令行参数。

## 交互式测试控制台 `relay_cli.py`

用于在**一个终端**内管理多条命名连接：建连/断连、**注册/登录**、心跳、**DATA** 三种模式、**CONTROL**（需管理员连接）、`flood` 压测、`admin` 拉取观测接口。

### 连接已有服务

```bash
export PYTHONPATH=tools
python3 tools/relay_cli.py --host 127.0.0.1 --port 19000 --admin-port 19003
```

### 一键拉起本地 `build/asio_forwarder`（随机端口）

```bash
export PYTHONPATH=tools
python3 tools/relay_cli.py --spawn-server
```

启动后会打印 `client` 与 `admin` 端口；`--spawn-server` 使用 `configs/dev/forwarder.json` 为模板（可用 `--config` 指定其他 JSON）。

### 常用命令（进入后输入 `help` 查看全部）

| 类别 | 示例 |
|------|------|
| 连接 | `connect a` / `close a` / `list` |
| 登录 | `login a user alice secret123`（登录） / `login a admin bob pw register`（注册管理员，末尾 `register`） |
| 心跳 | `hb a` |
| 转发 | `send a unicast <dst_username> <dst_conn_id> hello`；`send a broadcast <dst_username> x`；`send a round_robin <dst_username> 10 x` |
| payload | 普通 UTF-8 文本、`@/path/to/file` 二进制、`hex:0011aa...` |
| 控制 | `ctl adm list_users` / `ctl adm kick_user <target_uid>`（**需管理员账号那条连接**） |
| 统计 | `stats` / `stats a` |
| admin | `admin health` / `admin stats` / `admin events` / **`admin users`** |
| 压测 | `flood a unicast <dst_username> <dst_conn_id> 20000 32 256`；`flood a broadcast <dst_username> 20000 32 256`；`flood a round_robin <dst_username> 10 20000 32 256` |

`flood` 的 **RTT** 定义为：发出带 `seq` 的帧到收到对应 **201 SERVER_REPLY** 的往返时间（含 Python 与网络栈开销）。

## Web 前端交互界面（本地）

用于“最终呈现”与演示全部功能（注册/登录、DATA、CONTROL、KICK），通过桥接服务连接 TCP 中继：

```bash
pip install -r tools/requirements-relay.txt -r tools/requirements-webui.txt
export PYTHONPATH=tools
python3 tools/webui_server.py --relay-host 127.0.0.1 --relay-port 19000
```

打开 `http://127.0.0.1:8080`。

## Python 工具的定位（说明）

仓库中部分 Python 脚本（`tools/relay_cli.py`、`tools/relay_test_suite.py` 等）定位是 **联调/自测/演示工具**：它们通过 TCP 协议与 C++ 中继交互（并不会“运行 C++”）。
若你的生产交付要求“全 C++”，可仅交付 `asio_forwarder` + 相关协议文档与 **C++ 客户端封装 + C++ 真实场景示例**；Python 可以仅作为研发工具保留。

## C++ 真实业务模拟（交付用）

见 `examples/realistic_scenario_cpp/`，包含 `dispatcher_cpp` / `worker_cpp` / `admin_cpp` 三个可执行程序，使用 `asio_forwarder_sdk` 封装接口完成端到端演示。

## 压测数据样例（loopback，旧命令形态下的历史跑数）

环境：`relay_cli.py --spawn-server`，多 TCP、多 `user_id`。**当前 CLI 已改为账号登录与 `msg_type`**，下表为**改版前**一次跑数，仅作数量级参考；复测请用新版 `flood` 命令。

| 场景 | payload | count | window | 发送速率（约） | ACK RTT p50/p95/p99 (ms) |
|------|--------:|------:|-------:|------------------------:|--------------------------:|
| unicast → 单接收者 | 32 B | 20,000 | 256 | ≈ 7,363 | ≈ 1 / 7 / 44 |
| unicast | 1,024 B | 10,000 | 128 | ≈ 6,700 | ≈ 2 / 10 / 15 |
| unicast | 65,536 B | 1,000 | 32 | ≈ 4,478 | ≈ 3 / 6 / 12 |
| broadcast（旧语义：多其他用户） | 32 B | 20,000 | 256 | ≈ 3,076 | ≈ 75 / 92 / 101 |
| round_robin（旧语义） | 32 B | 20,000 | 256 | ≈ 4,064 | ≈ 66 / 86 / 93 |

同次运行中，`GET /api/stats` 在重载阶段可见 **`drops: 0`**、`pending_bytes: 0`（与当前默认 `flow.send_queue` 配置及负载匹配时）。

## 与实现的对应关系

- 协议与帧格式：`docs/protocol.md`、`include/fwd/protocol.hpp`、`include/fwd/relay_constants.hpp`
- 业务处理：`src/main.cpp` 中 `RelayServer::handle_client_frame`
- 配置键：`include/fwd/config.hpp`、`configs/dev/forwarder.json`；`load_config_or_throw` 在 `src/main.cpp`
