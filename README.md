# asio-forwarder（简单版：一进多出转发）

这是一个 **C++20 + Boost.Asio** 的最小可用 TCP 转发器骨架：

- **上游（1条连接）**：收二进制帧（小端 Header v2 40B + Body）
- **下游（多条连接）**：对每个下游连接 **原样广播** 转发
- **稳定性优先**：协议校验、最大包长限制、读/空闲超时、下游发送队列硬阈值保护、基础 metrics 日志

## 3 分钟跑通（推荐路径）

### 1) 编译 + 启动转发器

```bash
cd /home/xuanrui/asio-forwarder
./scripts/build.sh
./scripts/run_dev.sh
```

默认配置 `configs/dev/forwarder.json`（文件内带 `_comment` 说明字段）：

- upstream listen：`0.0.0.0:19001`（**仅 1 条上游连接槽位**）
- downstream listen：`0.0.0.0:19002`（可 accept 多条下游连接）
- admin（只读 HTTP）:`127.0.0.1:19003`（`/api/stats`、`/api/events`）

### 2) 终端自测（不依赖 Web）

终端 A：连下游收包（会阻塞等待）

```bash
python3 tools/downstream_recv.py --host 127.0.0.1 --port 19002
```

终端 B：模拟上游发一帧

```bash
python3 tools/upstream_send.py --host 127.0.0.1 --port 19001 --type 100 --seq 1 --text "你好，hello-binary"
```

你会看到下游打印收到的 header 字段与 body 预览。

### 3) Protobuf 载荷自测（`WireMsgType` + 多种 Body）

转发器仍 **不解析** Body；以下为 C++ / Python 序列化后走同一帧格式。约定见 `docs/protobuf.md`（`msg_type` 100/101/102）。

终端 A：下游收帧（`--count 0` 表示一直收直到上游断连）

```bash
./build/proto_recv --host 127.0.0.1 --port 19002 --count 1
```

终端 B：上游发报价（默认 `--kind stock`，自动 `msg_type=100`）

```bash
./build/proto_send --host 127.0.0.1 --port 19001 \
  --symbol 600000.SH --price 10.5 --volume 1000000 --exchange SSE
```

其它示例：`--kind heartbeat`、`--kind envelope --inner quote`。Python：`./scripts/gen_proto_python.sh` 后 `pip install -r tools/requirements-proto.txt`，再 `python3 tools/upstream_send_proto.py --help`。

### 4) Web 上用 Protobuf 结构体发包/看字段

启动转发器并运行 Web sidecar 后：

- 打开 `http://127.0.0.1:8080/upstream.html`：在 `body 编码` 选择 `protobuf:StockQuote` / `protobuf:Heartbeat` / `protobuf:MarketPayload(oneof)`，填写字段后点击发送。
- 打开 `http://127.0.0.1:8080/downstream.html` 点“查看详情”：下游表格新增的 `proto_preview` 会显示解析出来的字段（sidecar 负责 decode）。

## 快速开始

### 依赖

- Ubuntu 22.04
- g++ (>=11)
- CMake
- Boost（需要 `boost_system`，你机器已安装 `libboost-all-dev`）
- Protobuf（`libprotobuf-dev`、`protobuf-compiler`；用于生成 `market.pb.*` 与工具 `proto_send` / `proto_recv`）

### 编译

```bash
cd /home/xuanrui/asio-forwarder
./scripts/build.sh
```

### 自动测试（端到端）

在 **`build/` 目录下**运行 CTest：会执行 `scripts/test_e2e.sh`，为本机 **随机挑选空闲端口** 生成临时配置，启动 `asio_forwarder`，再用 C++/Python 工具验证 **v2 帧头** 与 **admin `/api/health`**（避免与已占用的 19001/19002 冲突）。

```bash
cd /home/xuanrui/asio-forwarder/build
ctest --output-on-failure
```

也可直接：`./scripts/test_e2e.sh`（需已 `./scripts/build.sh`）。

### 运行

```bash
./scripts/run_dev.sh
```

## Web 监控大屏与网页收发入口（sidecar）

项目自带一个独立的 Web sidecar（Node.js），用于：

- **监控大屏**：连接数、吞吐、drops、下游待发送队列总量、事件流
- **上游发包页**：在网页里按协议字段发送一帧
- **下游收包页**：通过 SSE 实时展示下游收到的帧（header + body 预览）
  - body 预览：utf8/hex 两种模式；utf8 模式 **最多展示 4KB** 且做 **UTF-8 边界安全截断**，避免中文乱码

### 启动转发器（包含 admin 端口）

```bash
cd /home/xuanrui/asio-forwarder
./scripts/build.sh
./scripts/run_dev.sh
```

admin 默认只监听本机：`http://127.0.0.1:19003/api/stats`、`/api/events`

### 启动 Web sidecar

```bash
cd /home/xuanrui/asio-forwarder
./scripts/run_web.sh
```

默认端口：`http://127.0.0.1:8080`

### 打开页面

- 大屏：`http://127.0.0.1:8080/index.html`
- 上游发包：`http://127.0.0.1:8080/upstream.html`
- 下游收包：`http://127.0.0.1:8080/downstream.html`

### 下游连接池（sidecar 的“模拟下游”）

`/downstream.html` 页面管理的是 **sidecar 主动创建的下游 TCP 客户端连接**（用于模拟多个下游消费端）。

- 默认连接数：1
- 最大连接数：200（sidecar 内置上限）

### 重要提示（上游单连接）

转发器 **只允许 1 条上游连接**。Web sidecar 的“上游发包”为了能连续发送，会建立并保持一个上游 TCP 连接，这会占用该槽位，可能踢掉真实上游连接（调试模式下使用）。

### 环境变量（自定义端口）

如果你改了 `configs/dev/forwarder.json` 端口，可以用环境变量让 sidecar 连接到对应端口：

```bash
WEB_PORT=8080 \
FWD_UP_HOST=127.0.0.1 FWD_UP_PORT=19001 \
FWD_DOWN_HOST=127.0.0.1 FWD_DOWN_PORT=19002 \
FWD_ADMIN_HOST=127.0.0.1 FWD_ADMIN_PORT=19003 \
./scripts/run_web.sh
```

## 协议（简版）

详见 `docs/protocol.md`；路线图与规划能力见 `docs/architecture.md`。

- 固定 **40 字节** Header（**v2**，小端，含 `src_user_id` / `dst_user_id`） + Body
- 通过 `body_len` 处理粘包/半包（读满头，再读满 body）
- 与旧版 **v1（24B 头）不兼容**，客户端与工具需一并升级

## 行为说明（简单版的稳定性策略）

- **上游连接**：
  - 仅允许同时存在 **1 条上游连接**；新上游连入会关闭旧连接。
  - 上游每次读取 Header/Body 都有 `timeouts.read_ms` 读超时；超时会取消 socket 读并关闭连接。
  - 若持续无数据超过 `timeouts.idle_ms`，视为“假死/空闲”，会关闭上游连接（等待下次上游重新连入）。
- **下游连接**：
  - 写失败（例如对端断开导致 `Broken pipe`）会立即 `close()`，并在后续广播/metrics 周期中自动清理。
  - 每个下游连接维护发送队列：
    - 超过 `flow.send_queue.high_water_bytes`：按 `flow.send_queue.on_high_water` 处理（默认 `drop`：对该下游丢弃新消息；可选 `disconnect`）。
    - 超过 `flow.send_queue.hard_limit_bytes`：断开该下游，保护内存。

- **metrics 输出**：
  - 周期由 `metrics.interval_ms` 控制（默认 5000ms）。

## 代码入口速览（从哪里开始看）

- `src/main.cpp`：核心逻辑（配置加载、上游读帧、广播、下游背压、admin/stats/events）
- `proto/market.proto`：业务契约（`WireMsgType`、`StockQuote`、`Heartbeat`、`MarketPayload`）
- `include/fwd/frame_io.hpp`：Header+Body 组帧 / 读帧 POSIX 小工具
- `src/proto_send.cpp` / `src/proto_recv.cpp`：Protobuf 组帧 / 按 `msg_type` 分发解析
- `scripts/gen_proto_python.sh`、`tools/upstream_send_proto.py`：Python 侧发包
- `include/fwd/protocol.hpp`：协议 Header/Frame 定义（pack/unpack）
- `docs/design.md`：核心设计结构（分层/数据流/关键约束/可观测性/扩展点）
- `docs/architecture.md`：当前实现 vs 规划（状态机、多连接、Msgpack 等）
- `web/server.js`：Web sidecar（大屏 + 网页发包/收包 + SSE）
- `web/public/*.html`：前端页面（大屏/发包/下游连接池/连接详情）
- `tools/*.py`：纯终端自测工具（发包/收包）
- `scripts/*.sh`：构建与启动脚本

## 常见问题

- **Address already in use**
  - 说明端口被占用；改 `configs/dev/forwarder.json` 的端口，或用 `ss -lntup | grep :19001` 找出占用进程。
- **上游发包后，下游收不到**
  - 确认下游已连接到 19002；
  - 确认上游发送的 `magic/version/header_len` 正确；
  - 确认 `body_len` 不超过 `limits.max_body_len`。
- **日志出现 upstream read loop exit: End of file**
  - 代表上游对端主动关闭连接（例如自测脚本发完即断），通常是正常现象。

## 下一步扩展（后续再做）

- 更多 `.proto`、与 Header `msg_type` 的约定表；转发器内按需解析 Body（路由）
- 业务线程池分发（业务与 IO 分离）
- 转发规则（按 msg_type / topic / account 路由）
- TLS（加密与身份认证）

