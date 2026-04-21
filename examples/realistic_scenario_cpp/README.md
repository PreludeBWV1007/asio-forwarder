# C++ 真实业务模拟（交付用）

本目录提供一套“真实业务”模拟（全 C++），用于交付时展示如何把 `asio_forwarder` 当黑盒中间件使用，并覆盖功能：

- 注册/登录（user/admin）
- 心跳
- DATA：unicast / broadcast（到目标用户全部连接）/ round_robin（到目标用户全部连接）
- CONTROL（管理员）：list_users / kick_user
- KICK：被踢后读取原因并退出/重试策略（示例中直接退出）

进程角色：

- `dispatcher_cpp`：按用户名派发任务给 worker，并接收结果
- `worker_cpp`：接收任务、处理并回传结果
- `admin_cpp`：定时 list_users（可扩展为自动 kick）

## 构建

```bash
./scripts/build.sh
```

## 运行

先启动中继（新终端）：

```bash
./build/asio_forwarder configs/dev/forwarder.json
```

然后分别运行三个进程（不同终端）：

```bash
./build/dispatcher_cpp --host 127.0.0.1 --port 19000 --register --demo-broadcast --demo-round-robin

# 启动同一 username 的两个 worker 进程，形成“单用户多连接”，以便看到 broadcast / round_robin 的效果
./build/worker_cpp --host 127.0.0.1 --port 19000 --username worker1 --password worker1-pw --register
./build/worker_cpp --host 127.0.0.1 --port 19000 --username worker1 --password worker1-pw
./build/admin_cpp --host 127.0.0.1 --port 19000 --register
```

二次运行请去掉 `--register`（改为登录）。

## 你将看到什么

- **unicast**：dispatcher 发送 `Task`，worker 处理后回传 `TaskResult`（dispatcher 终端会打印 result）。
- **broadcast**：dispatcher 发 notice（broadcast），两个 worker 进程都会打印该 deliver（dst_conn_id 不同）。
- **round_robin**：dispatcher 发 notice（round_robin），两个 worker 进程会“轮流”收到（同样可通过 dst_conn_id 观察）。
- **结构体传输（示例）**：dispatcher 还会发送一个 `StockTick`（模拟券商行情推送），worker/dispatcher 会按 `kind` 反序列化还原并打印关键字段。

本示例的业务 payload 采用统一的 msgpack “多态信封”结构：`{"kind": <enum-int>, "type": <string>, "data": <object>}`。

- `kind`：用于接收端快速分发反序列化逻辑（枚举值见 `poly_messages.hpp`）
- `type`：携带类型名称（便于日志/前端展示）
- `data`：真正的业务对象（`Task/TaskResult/Notice` 等）

业务结构体依然通过 `MSGPACK_DEFINE` 做序列化与反序列化（见 `messages.hpp`）。

