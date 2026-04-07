# 核心设计说明（对等中继）

## 目标

- **对等客户端**：任意客户端连接同一端口，经 LOGIN 绑定 `user_id` 后互发消息。
- **状态机（业务）**：由 Body 内 **`op`** 区分 LOGIN / HEARTBEAT / TRANSFER / CONTROL（无单独「状态端口」TCP）。
- **稳定性**：帧长上限、读超时、空闲超时、每连接发送队列水位、心跳超时踢线。
- **可观测性**：admin HTTP（JSON）+ 周期 metrics 日志 + 事件环形缓冲。

## 非目标（当前）

- Web 大屏与浏览器内发包（已移除；如需可后续单独实现）。
- TLS、鉴权、持久化队列。
- 在服务器内解析 **200 DELIVER** 的 payload（保持透明字节）。
- **Header.flags** 驱动的复杂路由（flags 在头文件中定义预留，主路径以 msgpack Body 为准）。

## 组件

| 组件 | 职责 |
|------|------|
| `asio_forwarder`（`src/main.cpp`） | 单 client acceptor + 多 `TcpSession`；协程读帧；msgpack 解析与转发；admin acceptor |
| Python 工具（`tools/`） | 组帧/解析、自动化 e2e、套件测试、**交互式 `relay_cli.py`** |

## 关键取舍

1. **单业务端口**：简化防火墙与客户端配置；角色由 LOGIN 的 `role` 与后续 `op` 表达。
2. **msgpack Body**：灵活；错误多以 **201** `ok:false` + `error` 字符串回复。
3. **DELIVER 原始 payload**：服务器不解析业务载荷，便于更换序列化格式。
4. **轮流发送**：`co_spawn` + `steady_timer`，不阻塞读循环。
5. **线程模型**：多线程共享一个 `boost::asio::io_context`，由 **`threads.io`** 控制；配置中的 **`threads.biz` 当前未接入执行路径**（仅被读取）。

## 已知行为与扩展

- **CONTROL 权限**：当前任意已登录会话可调用；若产品需要「仅 control 可踢人」，需在服务器增加校验。
- **路由扩展**：可按 `flags` 或新增 map 字段做 topic/组播语义。
- **Web**：可按本协议实现 LOGIN + 推送展示 **200/201**（SSE/WebSocket 等）。

## 测试与压测

见 **[`docs/testing.md`](testing.md)**（脚本、`relay_cli`、样例压测表）。
