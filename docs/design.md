# 核心设计说明（asio-forwarder）

本文档关注 **设计结构与分层**，刻意避开具体实现细节（协程/锁/容器等），用于帮助新同学快速建立“系统心智模型”。

## 目标与非目标

### 目标

- **一进多出**：1 条上游生产者连接，将收到的二进制帧 **原样广播** 给多个下游消费者连接。
- **稳定性优先**：在不可信网络/不可信上游/慢下游存在的情况下，保证进程可控、可观测、可恢复。
- **可观测性**：提供最小可用的统计与事件接口，便于 dashboard 与排障。

### 非目标（当前版本不做）

- 在转发器进程内做业务解码/编码（MsgPack/Protobuf 等；Body 对转发器仍为不透明字节）
- 按 topic/type 的复杂路由与订阅
- TLS、鉴权、租户隔离、持久化

## 组件与端口（角色划分）

系统由两部分组成：

### 1) Forwarder 本体（C++ / Boost.Asio）

- **upstream listen**：上游入口（TCP）
  - 设计为 **单连接槽位**（同一时刻只允许 1 条上游连接）
- **downstream listen**：下游入口（TCP）
  - 允许 **多连接并存**（每条连接都将收到广播帧）
- **admin（只读 HTTP）**：监控入口
  - `/api/stats`：统计快照（连接数/吞吐/drops/队列积压等）
  - `/api/events`：事件列表（连接变更/协议错误/超时等）

### 2) Web sidecar（Node.js）

sidecar 是“可视化与调试入口”，不参与业务转发链路本身：

- **dashboard**：轮询 sidecar `/api/stats`、`/api/events`（sidecar 再去拉 forwarder admin）
- **网页发包**：通过 sidecar 建立到 forwarder upstream 的 TCP 连接并写入一帧（调试用途）
- **网页收包**：sidecar 自己作为一个或多个下游客户端连接 forwarder downstream，接收广播后通过 **SSE** 推送给浏览器

> 注意：网页发包会占用 forwarder 的“唯一上游连接槽位”，因此只建议在调试时使用。

## 数据模型：Frame（协议帧）

Forwarder 处理的基本单位是 **Frame = Header(40B, v2) + Body(body_len)**：

- Header 为固定 40 字节（小端），包含 magic/version/header_len/body_len/msg_type/flags/seq/**src_user_id**/**dst_user_id**。
- Body 为原始二进制载荷（Msgpack / Protobuf 等由业务约定）。

Header 的存在用于：

- **快速协议识别**（magic/version/header_len）
- **解决粘包/半包**（body_len：先读满 header，再读满 body）
- **扩展预留字段**（msg_type/flags/seq）

协议细节参见 `docs/protocol.md`；路线图参见 `docs/architecture.md`。

## 核心数据流（从上游到下游）

### 上游读入

1. 接受一条 upstream TCP 连接（新连接会关闭旧连接）
2. 循环读取：
   - 读满 40B Header（带读超时）
   - 校验 Header（magic/version/header_len）
   - 校验 body_len（不得超过 `limits.max_body_len`）
   - 读满 body_len 字节 Body（带读超时）
3. 组装一帧（header+body）进入广播阶段

### 广播转发

将这一帧 **原样** 转发给所有当前在线下游连接：

- 每个下游连接独立维护发送队列
- 下游慢不会阻塞上游读循环；慢下游由背压策略“自我降级”（丢弃或断开）

## 关键约束与取舍（为什么这样设计）

### 1) 单上游连接槽位

选择单上游，是为了：

- 明确数据源（避免多源并发导致语义复杂化）
- 简化一致性与背压推导（只需要处理一种入流）
- 更适合作为“某个上游 gateway 的下游扇出层”

如果未来需要多上游，通常会引入：

- 上游身份/租户标识
- 合并策略（队列/优先级/限流）
- 更清晰的路由规则

### 2) 下游背压：按连接隔离

慢下游是常态（网络抖动、对端处理慢、对端暂挂等）。本设计选择：

- **只影响慢的那个下游**：超高水位时对该下游 drop/disconnect
- **保护进程**：硬上限时断开该下游，避免内存膨胀

背压阈值与策略通过配置项控制：

- `flow.send_queue.high_water_bytes`
- `flow.send_queue.hard_limit_bytes`
- `flow.send_queue.on_high_water`（`drop` 或 `disconnect`）

### 3) 超时：读超时 + 空闲超时

为避免“半开连接/对端卡死”导致协程或 socket 永久悬挂：

- `timeouts.read_ms`：每次读 Header/Body 的超时
- `timeouts.idle_ms`：长时间无数据的空闲超时（认为假死，主动断开等待重连）

## 可观测性（如何看系统是否健康）

Forwarder 的 admin 接口提供最小闭环：

- **连接**：上游是否在线；下游在线数
- **吞吐**：frames_in/bytes_in、frames_out/bytes_out
- **丢弃**：drops（常见来源：慢下游触发高水位策略、硬上限断开）
- **积压**：downstream_pending_bytes（所有下游待发送队列字节数总和）
- **事件**：/api/events（连接变更、协议错误、超时等）

sidecar 将这些数据做成：

- 大屏折线图（吞吐）
- 事件日志
- SSE 帧流（用于直观看到下游收到的 payload）

## 扩展点（后续怎么演进）

建议按“层”扩展，避免把业务逻辑塞进 IO 层：

- **协议层**：外层 Header 已为 **v2（40B）**；Body 推荐 Msgpack，Protobuf 仍可作为示例（`proto/market.proto`、`docs/protobuf.md`）
- **路由层**：按 msg_type/topic/account 做下游分发（从“全量广播”演进到“条件转发”）
- **安全层**：TLS + 鉴权 + 多租户隔离
- **性能层**：
  - 更细的 metrics（分下游维度）
  - 更稳健的事件系统（结构化、可过滤、可持久化）

