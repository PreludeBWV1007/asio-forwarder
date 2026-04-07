# 协议（v2 线格式 + Msgpack 业务体）

本文与 **`src/main.cpp`**、**`include/fwd/protocol.hpp`**、**`include/fwd/relay_constants.hpp`**、**`tools/relay_proto.py`** 对齐。若与旧版文档或外部说明冲突，以本仓库代码为准。

## 字节序

- 统一 **little-endian**。

## 传输帧（所有 TCP 连接相同）

`Header(40 bytes)` + `Body(body_len bytes)`。

布局与字段读写见 `proto::Header::pack_le` / `unpack_le`（`include/fwd/protocol.hpp`）。

| 字段 | 类型 | 说明 |
|---|---:|---|
| magic | u32 | `0x44574641` |
| version | u16 | `2` |
| header_len | u16 | `40` |
| body_len | u32 | 受配置 `limits.max_body_len` 约束；超限则断开 |
| msg_type | u32 | 客户端上行可为 0 等自定义；**服务器下发**见下节 |
| flags | u32 | 位定义在 `protocol.hpp`（`kBroadcast` 等）；**当前转发逻辑主要依据 Body `op`，未按 flags 解析路由** |
| seq | u32 | 建议单调；服务器回 **201** 时会回显该 `seq` |
| src_user_id | u64 | 逻辑用户 id；未登录可为 0 |
| dst_user_id | u64 | 可与 Body 内字段并用；**TRANSFER 单播**若 Body 未给 `dst_user_id`，可回退为 Header 的 `dst_user_id` |

### 服务器下发的 msg_type（`include/fwd/relay_constants.hpp`）

| 值 | 常量 | 含义 | Body |
|---:|---|---|---|
| 200 | `kMsgDeliver` | 投递对端 payload | **非 msgpack**：与发送方 TRANSFER 中 `payload` **相同的原始字节** |
| 201 | `kMsgServerReply` | 应答或错误 | **msgpack map**：LOGIN / HEARTBEAT / TRANSFER / CONTROL 的 `ok`、`error`、`op` 等 |

## Body（msgpack map，字符串键）

解析失败、非 map、缺 `op` 等会导致**断开连接**（见 `handle_client_frame`）。

### 未登录

**仅允许**：

```text
{ "op": "LOGIN", "user_id": <u64>, "role": "control" | "data" }
```

- `role` 缺省按 **`data`** 处理；非法值按 **`data`**。
- 成功后回复 **201**，字段包含 `ok`、`op:"LOGIN"`、`conn_id`、`user_id`、`role`。

### 已登录

任意已登录业务包会 **`touch_heartbeat()`**（不仅限于 HEARTBEAT）。

- **HEARTBEAT**：`{ "op": "HEARTBEAT" }`  
  - 回复 **201** `ok:true`、`op:"HEARTBEAT"`。  
  - 服务器未解析额外字段（如自定义 `ts_ms` 仅客户端自用）。

- **TRANSFER**：
  - `mode`：`"unicast"` | `"broadcast"` | `"round_robin"` | `"roundrobin"`（后两者等价）
  - `payload`：**bin 或 str**，透明出现在 **200** 帧 body 中
  - `dst_user_id`：**单播**时若 Body 未提供或为 0，则使用 **Header** 的 `dst_user_id`；仍为 0 则 **201** 报错
  - `interval_ms`：**轮流**时使用；`0` 表示使用配置 **`session.round_robin_default_interval_ms`**
  - 接受后回复 **201** `ok:true`、`op:"TRANSFER"`（**不保证对端应用已读**）

- **CONTROL**：
  - `action: "list_users"` → **201** 含 `users`（`user_id`、`connections`）
  - `action: "kick_user"` + `target_user_id` → 断开该用户所有连接，**201** 含 `kicked_count`

**安全说明（当前实现）**：**CONTROL 不校验 `role=="control"`**，任意已登录连接均可调用 `list_users` / `kick_user`。若需限制，应在业务层或后续在服务器增加鉴权。

### 每用户多连接（`user_deque_`）

- 同一 `user_id` 可多条 TCP；超过 **`session.max_connections_per_user`** 时**踢掉最老**连接。
- 新 **`control`** 登录会移除该用户已有的 **`control`** 连接，再入队。
- **投递目标**（`pick_preferred`）：**优先非 control 的在线连接**，否则任选在线连接。

## 配置项摘要

见 `configs/dev/forwarder.json` 与 `include/fwd/config.hpp`：

- `client.listen`（或兼容 `upstream.listen`）
- `admin.listen`、`admin.events_max`
- `threads.io`（**实际运行线程**）；`threads.biz` 可被读入配置但**当前未用于独立业务线程池**
- `timeouts.read_ms`、`timeouts.idle_ms`
- `limits.max_body_len`
- `flow.send_queue.high_water_bytes`、`hard_limit_bytes`、`on_high_water`（`drop` | `disconnect`）
- `metrics.interval_ms`
- `session.*`：`max_connections_per_user`、`heartbeat_timeout_ms`、`round_robin_default_interval_ms`、`broadcast_max_recipients`
