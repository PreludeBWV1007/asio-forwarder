# 版本更迭与历史说明

本文用于记录本仓库的“版本更迭/历史变化/与旧版差异”。**项目当前行为与接口以代码为准**，面向使用者的入口请看仓库根目录的 `README.md`。

## 当前版本的核心定位

- **对等 TCP 中继**：所有客户端连同一业务端口（`client.listen`），无固定“上游/下游”角色。
- **协议**：v2 线格式（40B 小端 header）+ 上行 msgpack map（`op` 驱动状态机）。
- **投递**：服务器向客户端下发两类帧：
  - **200 DELIVER**：Body 为 payload 原始字节
  - **201 SERVER_REPLY**：Body 为 msgpack map（ACK/错误/CONTROL 返回等）
- **观测**：admin 只读 HTTP（`/api/health`、`/api/stats`、`/api/events`）。

对应实现：`src/main.cpp`、`include/fwd/protocol.hpp`、`include/fwd/relay_constants.hpp`。

## 与旧版文档/实现的常见差异点

这些差异点是为了避免“拿旧版 README/文档去理解当前代码”导致误用。

- **无上游/下游二分**：当前只有 `client.listen` 一个业务端口，角色由 `LOGIN` + `op` 决定。
- **Body 为 msgpack**：业务状态机依赖 msgpack map 的 `op`（LOGIN/HEARTBEAT/TRANSFER/CONTROL）。
- **payload 透明转发**：服务器不解析 payload，200 的 Body 为原始字节。
- **Header.flags**：位定义存在于 `include/fwd/protocol.hpp`，但当前主路径路由仍以 Body 字段为主（未按 flags 做复杂路由）。
- **CONTROL 权限**：当前实现**不校验 `role=="control"`**；任意已登录连接可 `list_users` / `kick_user`（见 `docs/protocol.md` 的安全说明）。
- **线程配置**：JSON 里有 `threads.io` 与 `threads.biz`；当前进程实际只使用 `threads.io` 驱动 `io_context` 线程池，`threads.biz` 未接入独立业务线程池。
- **Web/sidecar**：旧方案中可能存在 Web 大屏/sidecar/Protobuf 等内容；当前仓库以现实现为准，若需可视化需按当前 msgpack 协议另行实现。

## 迁移建议（从旧客户端到新客户端）

- **先实现 v2 帧头**：40B 小端，校验 magic/version/header_len。
- **首包 LOGIN**：否则会被断开。
- **按 op 发送业务**：`HEARTBEAT`、`TRANSFER`（unicast/broadcast/round_robin）、`CONTROL`。
- **接收侧处理两类下行**：201 为 msgpack（ACK/错误/CONTROL 返回），200 为纯 payload 字节。

详细字段与示例见 `docs/protocol.md`；测试工具见 `docs/testing.md`。

