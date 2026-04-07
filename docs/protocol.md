# 协议（v2，稳定性优先）

## 字节序

- 统一使用 **little-endian**（小端）。

## Frame

一个消息帧 = `Header(40 bytes)` + `Body(body_len bytes)`。

> **破坏性变更**：v1 为 24 字节头 + `version=1`；当前实现与工具链仅支持 **v2 / 40 字节头**。

### Header（40 bytes）

| 字段 | 类型 | 说明 |
|---|---:|---|
| magic | u32 | 固定 `0x44574641`（用于快速识别协议） |
| version | u16 | 固定 `2` |
| header_len | u16 | 固定 `40` |
| body_len | u32 | body 长度；线上实际允许的最大值由配置 `limits.max_body_len` 约束（`body_len` 本身为 u32，理论可达约 4GiB−1） |
| msg_type | u32 | 业务消息类型（LOGIN / TRANSFER / CONTROL 等由业务枚举约定） |
| flags | u32 | 标志位（见下表） |
| seq | u32 | 序列号（建议每连接单调递增，便于对账与排障） |
| src_user_id | u64 | 发送方逻辑用户 id；未登录或系统帧可用 `0` |
| dst_user_id | u64 | 单播目标用户 id；`0` 表示「非单播」，具体语义结合 `flags`（例如广播/群组） |

### flags 位（预留，与 `include/fwd/protocol.hpp` 中常量一致）

| 位 | 常量名 | 含义 |
|----|--------|------|
| bit0 | `kBroadcast` | 广播意图（目标集合由业务或后续路由层解释） |
| bit1 | `kDstIsGroup` | `dst_user_id` 表示群组/主题 id，而非单播用户 |
| bit2 | `kNeedAck` | 期望对端回执（业务层；转发器当前可忽略） |

物理地址（IP:端口）**不放在每帧头里**；记在**连接元数据**中供日志关联。

### Body

- 原样转发的二进制载荷；转发器默认**不解释**语义。
- 推荐业务序列化：**MessagePack**（灵活 schema；版本与字段约定由业务维护）。
- 兼容示例：仍可使用 **Protobuf** 序列化后的 bytes（见 `proto/market.proto` 与 `docs/protobuf.md`）；此时 `msg_type` 可与 `WireMsgType` 对齐。
- 若 Body 内再带可读的用户名字符串等，应以 **Header 中的 `src_user_id` / `dst_user_id` 为准做路由**，避免转发器解析 Msgpack。

## 配置与实现的默认约束（运维口径）

| 项 | 默认/建议 |
|----|-----------|
| `limits.max_body_len` | 默认 **64MiB**（`include/fwd/config.hpp`），配置可调，校验上限 **1GiB** |
| 下游发送队列 | `flow.send_queue.high_water_bytes` / `hard_limit_bytes`：默认 **64MiB** / **256MiB**；策略 `drop` 或 `disconnect` |

详见 `docs/architecture.md` 与 `configs/dev/forwarder.json`。
