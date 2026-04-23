# 测试与压测说明

如何跑 **ctest / e2e / 套件 / `relay_cli` / WebUI**；文末为**历史 loopback 压测表**（旧广播语义下的一次跑数，**仅作数量级参考**，以本机复现为准）。

## 测试资产清单（本仓库里「算测试/回归」的源文件）

| # | 路径 | 在自动化里的位置 | 主要验证什么（被测侧） |
|---|------|------------------|------------------------|
| 1 | `scripts/test_e2e.sh` | `ctest` 唯一用例 `e2e_forwarder` 的入口；亦可直接执行 | 临时端口、起服、串起下表 2–4、最后 `GET /api/health` |
| 2 | `tools/relay_e2e_runner.py` | 由 `test_e2e.sh` 调用 | **服务端** `src/main.cpp` 路由：注册/登录、HEARTBEAT、DATA 单播、**200/201** 与 DELIVER 元数据（**不经 C++ SDK**） |
| 3 | `tools/relay_test_suite.py` | 由 `test_e2e.sh` 调用 | 多连接、单播/按用户广播/轮询、CONTROL、KICK、异常包等更宽**协议面**（同样针对服务端 + `relay_proto` 组帧） |
| 4 | `examples/asio_forwarder_client_smoke/smoke.cpp` | 若已编译出 `build/asio_forwarder_client_smoke`，`test_e2e.sh` 会 **`--connect` 到本次临时中继** 再跑**一轮**；也可单独跑（见下） | **C++ SDK**：`include/fwd/asio_forwarder_client.hpp` + `src/asio_forwarder_client.cpp`（及底层 `relay_client`）；探针/吞吐段不重复覆盖 Python 脚本的逐条用例，而是偏集成与性能形状 |
| — | `tools/relay_proto.py` | 被 2、3 import | **不单独计为一条「测试用例」**：协议常量/组帧/收包，与 `include/fwd/protocol.hpp` 等对齐，供黑盒脚本使用 |

`CMake` 里**没有**为 smoke 再写一条 `add_test`；但 **`./scripts/test_e2e.sh` 已内嵌**对 `asio_forwarder_client_smoke` 的调用（有则跑、无则跳过）。若要单独拉长参数（如 `--stress-n` / 默认稳定性轮次），或进程内**自带起服**跑全量，需**手敲**可执行文件，见下表「C++ smoke 单独跑」。

## 测试分层：不是只有 smoke，也不只有 Python

| 层级 | 作用 | 典型入口 |
|------|------|----------|
| **CTest** | `e2e_forwarder` → `test_e2e.sh`：**Python 两段 +（可选）C++ smoke 一段 +** admin health | `cd build && ctest --output-on-failure` |
| **C++ smoke 单独跑** | 不连 `test_e2e` 的短参数、或**无** build 时补跑 | `build/asio_forwarder_client_smoke`（`--connect` 或默认进程内起服，见 `--help`） |
| **手工 / 联调** | `relay_cli`、Web 桥、长联调 `run_scenario.py`、多进程示例 | 同下文各节 |

## 回归验证记录（可随发布更新本表）

以下为**同一代码树**在开发机上的一次结果；**不保证**等价于你方 CI/真机，仅说明「本仓库曾这样绿过」。

| 日期 | 步骤 | 结果（摘要） |
|------|------|----------------|
| 2026-04-23 | `cd build && ctest --output-on-failure` | `e2e_forwarder` **Passed**，总时间约 **122 s**（含启动中继 + Python e2e + 套件 + health） |
| 2026-04-23 | `./build/asio_forwarder_client_smoke --stress-n 200 --stability-rounds 5` | 输出 **`smoke: OK`**；`[throughput] n=200` 当次约 **9843 ms**；可靠性探针强断言**通过**（若有 `[probe] 观察` 行，为协议/语义说明，不表示失败） |
| Web 桥 | — | **无**自动化用例；依赖人工安装依赖、起 `webui_server.py`、浏览器点一点功能。交付清单仍将其列为**联调/演示**组件。 |

## 与自动化测试无直接关系的文件/目录（一般不要当「测完可删的废件」处理）

| 类型 | 路径示例 | 说明 |
|------|----------|------|
| 主程序与库 | `src/main.cpp`，`src/relay_client.cpp`，`src/asio_forwarder_client.cpp` | 被 e2e / smoke **验证**，不是「测试多出来的文件」。 |
| 协议与配置 | `include/fwd/*`，`configs/*.json` | 运行与联调需要。 |
| 联调工具（**非 ctest 步骤**） | `tools/relay_cli.py`，`tools/webui_server.py` + `tools/webui_static/`，`tools/run_scenario.py` | 人工或半自动；删除会丢掉交付/文档里常提到的演示路径。 |
| 交付示例（**非 ctest 步骤**） | `examples/realistic_scenario_cpp/*`，`examples/realistic_scenario/*`（旧版 Python 三进程） | 展示多进程/多态 payload；`smoke.cpp` 仅 **include** 了 `realistic_scenario_cpp/messages.hpp`，**不要**删该头除非改 smoke 依赖。 |
| 文档 | `docs/*.md` | 含本文 `testing.md`。 |

**是否可删？** 只有在你明确**不再需要**某条联调故事（例如只保留 C++ 示例、不要 Python 三进程 `examples/realistic_scenario/`）时，再删对应树；**不要**为「给测试让路」而删主程序、协议或 `relay_proto.py`。删除前建议全仓库 `grep` 引用并跑一遍 `ctest` + 手跑 smoke。

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
