# 数据流：从 TCP 到 DATA 投递

主逻辑在 **`src/main.cpp`**：`TcpSession::read_loop`、`RelayServer::handle_client_frame`。

## 1. 连接

1. `client_acceptor_.async_accept` → 分配 **`conn_id`** → 加入 **`all_`**、**`by_id_`**。
2. `co_spawn` 启动 **`read_loop`**：循环「**带超时的**读 40B 头 → 校验 magic/version/header_len → 读 `body_len`」。
3. 若配置 **`timeouts.idle_ms > 0`**，整条连接长期无成功读帧则空闲超时 → **`notify_and_close`**（**202 KICK** + 断开）。

## 2. 首包必须为 login（`msg_type = 1`）

- 解析 msgpack map：`username`、`password`、`peer_role`、`register`。
- **`try_authenticate`**：注册或校验登录；维护 **`accounts_`** / **`uid_to_username_`**；将会话加入 **`user_deque_[user_id]`**；超限则对最旧连接 **`notify_and_close`**。
- 回复 **201** msgpack（含 **`conn_id`**、`user_id`、`username`、`peer_role` 等）。

## 3. 已登录

- **`msg_type == 2`（HEARTBEAT）**：`touch_heartbeat()`，回复 **201**。
- **`msg_type == 4`（DATA）**：
  - 解析 `mode`、`dst_username`、`dst_conn_id`（unicast）、`payload`、`interval_ms`（round_robin）。
  - 向对端构造 **200**：`Header.msg_type = kMsgDeliver`，并约定 `Header.src_user_id/dst_user_id` 置 0；**Body = msgpack** `{ payload, src_conn_id, dst_conn_id, src_username, dst_username }`。
  - 单播 / 广播到用户全连接 / 对该用户连接轮询：见 `docs/protocol.md`。
  - 成功后对发送方回复 **201** `op:"DATA"`（表示服务器已受理；**不保证对端已读**）。
- **`msg_type == 3`（CONTROL）**：校验 **`is_admin_account()`**；`list_users` / `kick_user`；踢人时对受害者 **`notify_and_close`**（先发 **202**）。

已登录帧在 `handle_client_frame` 内会 **`touch_heartbeat()`**（**HEARTBEAT** 分支与其它 **DATA/CONTROL** 分支均会刷新）。

## 4. 发送路径

- **`TcpSession::send_frame`**：投递到连接 **strand** 上的队列 → **`async_write`** 串行写出。
- **`kick_notify_and_close`**：清空队列、排队 **202** 帧，写完后关闭 socket。
- 队列字节数超过 **`flow.send_queue`** 阈值时 drop 或 disconnect（见配置）。

## 5. 断开

- `read_loop` 结束 → **`remove_session`**：从 **`all_`**、**`by_id_`**、**`user_deque_`** 摘除。

## 相关文件

- `include/fwd/protocol.hpp`：v2 头
- `include/fwd/relay_constants.hpp`：客户端 1–4、200 / 201 / 202
- `include/fwd/sha256.hpp`：口令摘要
- `include/fwd/config.hpp`、`configs/dev/forwarder.json`
- `tools/relay_proto.py`、`tools/relay_cli.py`
