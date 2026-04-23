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
| msg_type | u32 | **客户端上行**：见下节「客户端 msg_type」；**服务器下行**：见「服务器下发的 msg_type」 |
| flags | u32 | 位定义在 `protocol.hpp`（`kBroadcast` 等）；**当前转发逻辑不依赖 flags 路由** |
| seq | u32 | 建议单调；服务器回 **201** 时会回显该 `seq` |
| src_user_id | u64 | 保留字段。**当前实现不用于路由**，服务器下行约定填 `0`；客户端可忽略 |
| dst_user_id | u64 | 保留字段。**当前实现不用于路由**，服务器下行约定填 `0`；客户端可忽略 |

### 客户端上行：`Header.msg_type`（`include/fwd/relay_constants.hpp`）

| 值 | 常量 | 含义 | Body |
|---:|---|---|---|
| 1 | `kClientLogin` | 注册或登录 | **msgpack map**，见「登录 / 注册」 |
| 2 | `kClientHeartbeat` | 心跳 | **msgpack map**（可为空 map `{}`） |
| 3 | `kClientControl` | 管理控制 | **msgpack map**，见「CONTROL」 |
| 4 | `kClientData` | 数据转发 | **msgpack map**，见「DATA」 |

**未登录**时：仅允许 **`msg_type == 1`** 的登录帧；其它类型会被断开。

**已登录**时：仅处理 **2 / 3 / 4**；其它类型返回 **201** `ok:false` 说明错误（不断开，除非协议严重错误）。

### 服务器下发的 msg_type

| 值 | 常量 | 含义 | Body |
|---:|---|---|---|
| 200 | `kMsgDeliver` | 投递对端数据 | **msgpack map**：`payload`（bin）、`src_conn_id`（u64）、`dst_conn_id`（u64）、`src_username`（str）、`dst_username`（str）。`payload` 为发送方在 **DATA** 里给出的原始字节，服务器不解释其业务语义 |
| 201 | `kMsgServerReply` | 应答或错误 | **msgpack map**：见下「201 SERVER_REPLY」 |
| 202 | `kMsgKick` | 服务端即将断开本连接 | **msgpack map**：`op:"KICK"`、`reason`（字符串，说明断开原因） |

### 201 `SERVER_REPLY`

- **成功或常规结果**：一般含 `ok:true`，并带 `op`（如 `LOGIN` / `HEARTBEAT` / `DATA` / `CONTROL` 等，随场景而定）及业务字段；回显的 `seq` 与本次上行帧一致。
- **仅表示拒绝/错误**（`handle_client_frame` 内 `send_err_reply`）：`ok:false` + `error`（人可读说明）。这类应答**不保证**带 `op` 字段。客户端/解析器在匹配某次请求时，应以「同 `seq` 且 `ok:false`」作为错误主路径，**不要**仅依赖 `op` 与请求类型一致来识别失败。

## 登录 / 注册（`msg_type = 1`）

Body 为 **msgpack map**（字符串键）：

```text
{
  "username": <str>,
  "password": <str>,
  "peer_role": "user" | "admin",
  "register": <bool>
}
```

- **`register: true`**：注册新账号。用户名已存在则 **201** `ok:false`（不断开）。
- **`register: false`**：登录已有账号。校验密码；并校验本次 **`peer_role` 与账号创建时保存的权限一致**（普通用户 / 管理员）。
- 口令存储：进程内使用 **盐（hex）+ SHA-256（hex）** 摘要（见 `include/fwd/sha256.hpp`）；**重启后账号清空**。
- 成功后服务器分配 **`user_id`（u64）** 并将会话加入该用户的连接表；登录成功回复 **201**，字段包含：  
  `ok`、`op:"LOGIN"`、`conn_id`、`user_id`、`username`、`peer_role`（`"user"` 或 `"admin"`）。

### 每用户多连接（`user_deque_`）

- 同一 `user_id` 可多条 TCP；超过 **`session.max_connections_per_user`**（默认 8）时**淘汰最旧**连接，并对其发送 **202 KICK**（原因说明）后断开。
- 不再有历史上的 **`control` / `data` 连接角色**；管理员与普通用户的区别在 **账号 `peer_role`**，见 CONTROL 一节。

## 已登录：心跳（`msg_type = 2`）

- Body：**msgpack map**（可为 `{}`）。
- 任意已登录业务包（含本心跳）会 **`touch_heartbeat()`**。
- 回复 **201**：`ok:true`、`op:"HEARTBEAT"`。

## 已登录：DATA（`msg_type = 4`）

Body 为 **msgpack map**：

- **`mode`**：`"unicast"` | `"broadcast"` | `"round_robin"` | `"roundrobin"`（后两者等价）
- **`payload`**：**bin 或 str**，透明放入对端 **200** 的 `payload` 字段
- **`dst_username`**（str）  
  - **unicast**：必填。表示目标接收者用户名  
  - **broadcast**：必填。表示目标接收者用户名；服务器向该用户下**全部在线连接**各发一条 **200**  
  - **round_robin**：必填。表示目标接收者用户名；服务器对该用户全部在线连接按顺序、间隔 **`interval_ms`** 毫秒逐条投递 **200**
- **`dst_conn_id`**（u64，可选）：**仅 unicast** 有效；`0` 表示由服务器在该用户下任选一条在线连接；非 `0` 则投递到 `conn_id` 匹配的连接
- **`interval_ms`**：**round_robin** 使用；`0` 表示使用配置 **`session.round_robin_default_interval_ms`**
- 受理后回复 **201**：`ok:true`、`op:"DATA"`（**不保证对端应用已读**）

### 业务 payload 推荐约定：`{type, data}`（强烈建议）

由于 `payload` 在传输层是**不透明字节**（服务器不解析），但业务侧通常需要在同一条链路上承载多种 `struct`/对象类型，推荐把 `payload` 统一约定为一个 **msgpack map**：

```text
{
  "type": <str>,   // 业务类型标签（字符串）
  "data": <obj>    // 与 type 对应的业务对象（可为 map/array/number/...）
}
```

说明：

- **接收端必须先知道目标类型** 才能把 `data` 反序列化为 C++ `struct`；因此 `type` 是用于运行时分发的关键字段（例如 switch/if + `MSGPACK_DEFINE`）。
- 若不使用 `type`，接收端只能得到“无类型”的 `msgpack::object`/map，需要业务侧自己约束“这一条链路只会收到某一种类型”，否则很难安全地还原为具体 C++ 类型。
- 本仓库的 C++ SDK 在接收 **200 DELIVER** 时会对 `payload` 做 best-effort 解码：若满足上述 `{type,data}`，会在 `Deliver.typed` 中给出 `type` 与 `as<T>()` 转换入口。

## 已登录：CONTROL（`msg_type = 3`）

**仅 `peer_role` 为管理员（`admin`）的账号**可调用；否则 **201** `ok:false`。

Body 为 **msgpack map**：

- **`action: "list_users"`** → **201** 含 `users`：每项含 `user_id`、`username`、`connections`（数组，元素为 `conn_id`、`endpoint`）
- **`action: "kick_user"`** + `target_user_id` → 向该用户**所有在线连接**先发 **202 KICK**（含具体 `reason`），再断开；**201** 含 `ok`、`op:"CONTROL"`、`kicked_count`

解析失败、非 map、未登录发业务帧等会导致**断开连接**（见 `handle_client_frame`）。

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

## Admin HTTP（只读 JSON）

除健康/统计/事件外，提供用户与连接映射快照：

- **`GET /api/users`** → `{"users":[{"user_id", "username", "connections":[{"conn_id","endpoint"}]}]}`

其余：`/api/health`、`/api/stats`、`/api/events`（见 `README.md` / `docs/architecture.md`）。
