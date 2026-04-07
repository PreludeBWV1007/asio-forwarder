# 数据流：从 TCP 到 TRANSFER 投递

主逻辑在 **`src/main.cpp`**：`TcpSession::read_loop`、`RelayServer::handle_client_frame`。

## 1. 连接

1. `client_acceptor_.async_accept` → 分配 **`conn_id`** → 加入 **`all_`**、**`by_id_`**。
2. `co_spawn` 启动 **`read_loop`**：循环「**带超时的**读 40B 头 → 校验 magic/version/header_len → 读 `body_len`」。
3. 若配置 **`timeouts.idle_ms > 0`**，整条连接长期无成功读帧则空闲超时断开。

## 2. 首包必须为 LOGIN

- 解析 msgpack map，**`op == "LOGIN"`**。
- **`apply_login`**：维护 **`user_deque_[user_id]`**，control 替换同用户旧 control、总数超限踢最老。
- 回复 **201** msgpack（含 **`conn_id`**、`user_id`、`role` 等）。

## 3. 已登录

- 任意业务包调用 **`touch_heartbeat()`**（刷新心跳截止时间）。
- **TRANSFER**：
  - 解析 `mode`、`dst_user_id`（可回退 Header）、`payload`（bin/str）、`interval_ms`。
  - 向对端构造 **200**：`Header.msg_type = kMsgDeliver`，`src_user_id`/`dst_user_id` 填发送方与目标用户，**Body = payload 原始字节**。
  - 单播：`pick_preferred(dst_user_id)`；广播：其他在线用户各一条（有上限）；轮流：异步定时逐用户发送。
- 成功后对发送方回复 **201** `op:"TRANSFER"`（表示服务器已受理；**不保证对端已读**）。

## 4. 发送路径

- **`TcpSession::send_frame`**：投递到连接 **strand** 上的队列 → **`async_write`** 串行写出。
- 队列字节数超过 **`flow.send_queue`** 阈值时 drop 或 disconnect（见配置）。

## 5. 断开

- `read_loop` 结束 → **`remove_session`**：从 **`all_`**、**`by_id_`**、**`user_deque_`** 摘除。

## 相关文件

- `include/fwd/protocol.hpp`：v2 头
- `include/fwd/relay_constants.hpp`：200 / 201
- `include/fwd/config.hpp`、`configs/dev/forwarder.json`
- `tools/relay_proto.py`、`tools/relay_cli.py`
