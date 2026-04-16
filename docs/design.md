# 核心设计说明（对等中继）

## 目标

- **对等客户端**：任意客户端连接同一端口，经 **login(1)** 注册或登录后互发消息。
- **帧类型驱动**：由 **`Header.msg_type`** 区分 login / heartbeat / control / data（不再依赖 Body 内 `op` 作为主路由）。
- **用户与连接**：外部以 **`user_id` / `username`** 为粒度；内部以 **`conn_id`** 区分同一用户的多条 TCP（上限见配置）。
- **稳定性**：帧长上限、读超时、空闲超时、每连接发送队列水位、心跳超时踢线（尽量带 **202 KICK** 原因）。
- **可观测性**：admin HTTP（JSON）+ 周期 metrics 日志 + 事件环形缓冲 + **`/api/users`** 用户-连接映射快照。

## 非目标（当前）

- Web 大屏与浏览器内发包（已移除；如需可后续单独实现）。
- TLS、账户持久化（口令与账号仅在进程内存）。
- 在服务器内解析 **200 DELIVER** 中 **`payload` 的业务语义**（仅解析外层 msgpack 信封以携带 `src_conn_id` / `dst_conn_id`）。
- **Header.flags** 驱动的复杂路由（flags 在头文件中定义预留，主路径不依赖 flags）。

## 组件

| 组件 | 职责 |
|------|------|
| `asio_forwarder`（`src/main.cpp`） | 单 client acceptor + 多 `TcpSession`；协程读帧；账户与转发；admin acceptor |
| Python 工具（`tools/`） | 组帧/解析、自动化 e2e、套件测试、**交互式 `relay_cli.py`** |

## 关键取舍

1. **单业务端口**：简化防火墙与客户端配置；权限由账号 **`peer_role`（user/admin）** 表达。
2. **msgpack Body**：灵活；业务错误多以 **201** `ok:false` + `error` 字符串回复（登录失败等不断开）。
3. **DELIVER 信封**：在保持 `payload` 透明的前提下，附带 **`src_conn_id` / `dst_conn_id`** 与 **`src_username` / `dst_username`**，便于客户端选择连接与展示对端身份。
4. **轮流发送**：`co_spawn` + `steady_timer`，不阻塞读循环。
5. **线程模型**：多线程共享一个 `boost::asio::io_context`，由 **`threads.io`** 控制；配置中的 **`threads.biz` 当前未接入执行路径**（仅被读取）。

## 已知行为与扩展

- **CONTROL**：仅管理员账号；若需更细 RBAC，可扩展账户字段与校验。
- **路由扩展**：可按 `flags` 或 DATA map 内新增字段做 topic/组播语义。
- **Web**：可按本协议实现 login + 推送展示 **200/201/202**（SSE/WebSocket 等）。

## 测试与压测

见 **[`docs/testing.md`](testing.md)**（脚本、`relay_cli`、样例压测表；**压测命令形态已随协议更新**）。
