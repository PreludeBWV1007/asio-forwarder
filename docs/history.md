# 版本更迭与历史说明

本文用于记录本仓库的“版本更迭/历史变化/与旧版差异”。**项目当前行为与接口以代码为准**，面向使用者的入口请看仓库根目录的 `README.md`。

## 当前版本的核心定位

- **对等 TCP 中继**：所有客户端连同一业务端口（`client.listen`），无固定“上游/下游”角色。
- **协议**：v2 线格式（40B 小端 header）+ **msgpack body**；上行由 **`Header.msg_type`** 区分 **login(1) / heartbeat(2) / control(3) / data(4)**。
- **账户**：用户名 + 口令（注册/登录）+ **`peer_role`（`user` | `admin`）**；分配 **`user_id`**；**重启丢失**。
- **投递**：服务器向客户端下发帧类型包括：
  - **200 DELIVER**：msgpack：`payload` + `src_conn_id` + `dst_conn_id` + `src_username` + `dst_username`（下行 header 的 src/dst user_id 约定为 0）
  - **201 SERVER_REPLY**：msgpack（ACK/错误/CONTROL 返回等）
  - **202 KICK**：msgpack（断开原因，先于 TCP 关闭尽量送达）
- **观测**：admin 只读 HTTP（`/api/health`、`/api/stats`、`/api/events`、**`/api/users`**）。

对应实现：`src/main.cpp`、`include/fwd/protocol.hpp`、`include/fwd/relay_constants.hpp`、`include/fwd/sha256.hpp`。

## 与更早版本的差异（Body `op` + 旧 TRANSFER 时代）

若你仍持有「`op: LOGIN` + `user_id` + `role: data|control`」或「`op: TRANSFER`」类文档/客户端，请注意已变更：

| 主题 | 旧行为（概要） | 当前行为（概要） |
|------|----------------|------------------|
| 上行路由 | Body **`op`**（LOGIN/HEARTBEAT/TRANSFER/CONTROL） | **`Header.msg_type`** 1–4 + msgpack map 字段 |
| 登录 | `user_id` + `role: data\|control` | **`username` + `password` + `peer_role` + `register`**；服务器分配 `user_id` |
| 连接角色 | `control` / `data` 连接类型、control 替换逻辑 | **已移除**；管理员能力在账号 **`peer_role: admin`** |
| 数据转发 | `op: TRANSFER` | **`msg_type: data(4)`**，map 内 `mode` / **`dst_username`** / `payload` 等 |
| 广播 | 除发送方外**所有其他用户**各一条（每用户优选一条连接） | 必须指定 **`dst_username`**：向该用户**全部连接**扇出 |
| 轮询 | 对其他**用户**逐用户发 | 对目标 **`dst_username` 的全部连接**等间隔发送 |
| DELIVER body | 与 payload **相同原始字节** | **msgpack 信封**（含 `payload`、连接 id、用户名） |
| CONTROL | 任意已登录可调 | **仅管理员账号**可调 |
| 踢线 | 多直接 `stop` | 尽量先发 **202** 带 **`reason`** |

## 仍保留或需注意的点

- **Header.flags**：位定义存在于 `include/fwd/protocol.hpp`，当前主路径**不依赖 flags** 做路由。
- **线程配置**：JSON 里有 `threads.io` 与 `threads.biz`；当前进程实际只使用 `threads.io` 驱动 `io_context` 线程池，`threads.biz` 未接入独立业务线程池。
- **Web/sidecar**：旧方案中可能存在 Web 大屏/sidecar/Protobuf 等内容；当前仓库以现实现为准。

## 迁移建议（从旧客户端到新客户端）

- 实现 **v2 帧头**：40B 小端，校验 magic/version/header_len。
- **首帧**：`msg_type=1`，msgpack：`username`、`password`、`peer_role`、`register`。
- **已登录**：`msg_type=2` 心跳；`msg_type=4` 发数据（`mode`、`dst_username`、…）；`msg_type=3` 控制（仅管理员）。
- **接收**：处理 **200**（解 msgpack 取 `payload`）、**201**、**202**。

详细字段见 **`docs/protocol.md`**；Python 参考 **`docs/client_api.md`**、**`tools/relay_client.py`**；测试见 **`docs/testing.md`**。
