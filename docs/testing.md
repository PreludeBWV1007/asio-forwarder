# 测试与压测说明

本文描述如何运行自动化测试、Python 工具与交互式控制台，并记录一次在本仓库环境中测得的压测数据。**压测数值随 CPU、内核、是否 loopback、客户端实现语言而变**，仅作相对参考；以你本机复现为准。

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

# 最小 e2e：LOGIN + HEARTBEAT + TRANSFER 单播 + 校验 DELIVER
python3 tools/relay_e2e_runner.py 127.0.0.1 19000

# 套件：unicast / broadcast / round_robin / CONTROL / 非法 msgpack 断连等
python3 tools/relay_test_suite.py 127.0.0.1 19000
```

端口与 `configs/dev/forwarder.json` 中 `client.listen.port` 一致；若改配置请同步改命令行参数。

## 交互式测试控制台 `relay_cli.py`

用于在**一个终端**内管理多条命名连接：建连/断连、LOGIN、HEARTBEAT、三种 TRANSFER 模式、CONTROL、`flood` 压测、`admin` 拉取观测接口。

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
| 登录与心跳 | `login a 100 data` / `login c 300 control` / `hb a` |
| 转发 | `send a unicast 200 hello` / `send a broadcast x` / `send a round_robin 10 x` |
| payload | 普通 UTF-8 文本、`@/path/to/file` 二进制、`hex:0011aa...` |
| 控制 | `ctl d list_users` / `ctl d kick_user 200` |
| 统计 | `stats` / `stats a` |
| admin | `admin health` / `admin stats` / `admin events` |
| 压测 | `flood a unicast 200 20000 32 256`（dst、count、bytes、window） |

`flood` 的 **RTT** 定义为：发出带 `seq` 的帧到收到对应 **201 SERVER_REPLY** 的往返时间（含 Python 与网络栈开销）。

## 压测数据样例（loopback，开发机一次跑数）

环境：`relay_cli.py --spawn-server`，4 条 TCP（用户 100/200/300/400，其中一条为 `control`），其余为 `data`。压测命令形态见 `relay_cli.py` 的 `flood`。

| 场景 | payload | count | window | 发送速率（TRANSFER/s） | ACK RTT p50/p95/p99 (ms) |
|------|--------:|------:|-------:|------------------------:|--------------------------:|
| unicast → 单接收者 | 32 B | 20,000 | 256 | ≈ 7,363 | ≈ 1 / 7 / 44 |
| unicast | 1,024 B | 10,000 | 128 | ≈ 6,700 | ≈ 2 / 10 / 15 |
| unicast | 65,536 B | 1,000 | 32 | ≈ 4,478 | ≈ 3 / 6 / 12 |
| broadcast（约 3 个其他用户各 1 条 DELIVER/次） | 32 B | 20,000 | 256 | ≈ 3,076 | ≈ 75 / 92 / 101 |
| round_robin，`interval_ms=0`（同上多接收者） | 32 B | 20,000 | 256 | ≈ 4,064 | ≈ 66 / 86 / 93 |

同次运行中，`GET /api/stats` 在重载阶段可见 **`drops: 0`**、`pending_bytes: 0`（与当前默认 `flow.send_queue` 配置及负载匹配时）。

## 与实现的对应关系

- 协议与帧格式：`docs/protocol.md`、`include/fwd/protocol.hpp`、`include/fwd/relay_constants.hpp`
- 业务状态机：`src/main.cpp` 中 `RelayServer::handle_client_frame`
- 配置键：`include/fwd/config.hpp`、`configs/dev/forwarder.json`；`load_config_or_throw` 在 `src/main.cpp`
