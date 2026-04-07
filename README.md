# asio-forwarder（对等 TCP 中继）

`asio-forwarder` 是一个 **C++20 + Boost.Asio（协程）** 实现的 **对等（peer-to-peer）TCP 中继服务**：所有客户端连接同一业务端口，先 **LOGIN** 绑定 `user_id`，随后通过 **Msgpack 状态机**（`op`）进行心跳与消息转发。

协议：**v2 线格式（40B 小端 header）+ 上行 msgpack map**；服务器下行分两类：**200 DELIVER（纯 payload 字节）**、**201 SERVER_REPLY（msgpack ACK/错误/CONTROL 返回）**。详见 `docs/protocol.md`。

## 项目特征

- **单端口对等接入**：配置简单（仅 `client.listen`），无“上游/下游”固定角色。
- **透明转发**：服务器不解析业务 payload，200 的 Body 为原样字节。
- **稳定性保护**：最大 body 长度、读/空闲超时、心跳踢线、每连接发送队列背压（drop/disconnect）。
- **可观测性**：admin 只读 HTTP：`/api/health`、`/api/stats`、`/api/events`。
- **自测/压测工具齐全**：e2e、测试套件、交互式多连接控制台（支持 `flood`）。

## 架构与实现方式（简要）

- 进程入口：`src/main.cpp`
- 关键组件：
  - **`RelayServer`**：accept client/admin；维护 `user_id → deque<session>`；实现 TRANSFER 路由策略
  - **`TcpSession`**：协程读帧 + strand 串行写出；队列背压
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

## 某次压测数据（样例）

说明：以下为一次 `relay_cli.py --spawn-server` 的 loopback 运行结果（4 条 TCP：用户 100/200/300/400，其中一条为 control，其余为 data）。**数值与机器/内核/网络栈/客户端语言相关**，用于相对参考；建议你在目标环境复跑。

| 场景 | payload | count | window | 发送速率（TRANSFER/s） | ACK RTT p50/p95/p99 (ms) |
|------|--------:|------:|-------:|------------------------:|--------------------------:|
| unicast → 单接收者 | 32 B | 20,000 | 256 | ≈ 7,363 | ≈ 1 / 7 / 44 |
| unicast | 1,024 B | 10,000 | 128 | ≈ 6,700 | ≈ 2 / 10 / 15 |
| unicast | 65,536 B | 1,000 | 32 | ≈ 4,478 | ≈ 3 / 6 / 12 |
| broadcast（约 3 个其他用户各 1 条 DELIVER/次） | 32 B | 20,000 | 256 | ≈ 3,076 | ≈ 75 / 92 / 101 |
| round_robin，`interval_ms=0`（同上多接收者） | 32 B | 20,000 | 256 | ≈ 4,064 | ≈ 66 / 86 / 93 |

## 文档导航

- `docs/protocol.md`：协议与字段（v2 header、msgpack op、200/201）
- `docs/architecture.md`：组件与线程/配置注意点
- `docs/data_flow.md`：读帧 → 路由 → 写出数据流
- `docs/design.md`：取舍与扩展方向
- `docs/testing.md`：测试脚本、交互控制台、压测说明
- `docs/history.md`：**版本更迭/历史变化（与旧版差异）**

## 重要实现提醒（与直觉可能不同）

- **CONTROL 权限**：当前实现**不校验 `role==\"control\"`**，任意已登录连接可 `list_users` / `kick_user`（见 `docs/protocol.md`）。
- **Header.flags**：位定义存在，但当前主路由仍以 Body 字段为主，未按 flags 做复杂路由。

