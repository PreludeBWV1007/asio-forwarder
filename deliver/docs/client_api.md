# 生产交付：客户端接口（Python + C++ 入口）

**线协议与 200/201/202 细节以 [protocol.md](protocol.md) 为准。**

对业务 payload：将结构体/对象**序列化为 bytes**（msgpack、protobuf 等），作为 **DATA** 的 `payload` 发送；对端在 **200 DELIVER** 中取出 `payload` 再反序列化。服务端对 payload **不透明**。

## `RelayClient`（最小可用生产接口）

位置：`local/tools/relay_client.py`

特点：

- 一个 `RelayClient` 实例 = 一条 TCP 连接
- 提供自动心跳（可关）
- **`login(username, password, peer_role=..., register=...)`**（内部使用 `msg_type=1`）
- **`send_unicast` / `send_broadcast_to_user` / `send_round_robin_to_user`**（`msg_type=4`）
- **`control_*`**（`msg_type=3`，需管理员账号）
- 接收：`inbox_deliver`、`inbox_reply`、**`inbox_kick`**（或回调 `on_kick`）

### 用法示例（两端）

#### 接收端（先注册或登录）

```python
import sys

sys.path.insert(0, "tools")

from relay_client import RelayClient

c = RelayClient("127.0.0.1", 19000, hb_interval_s=5)
c.connect()
seq = c.login("bob", "bob-secret", peer_role="user", register=True)
print("login sent seq=", seq)
# 等待 LOGIN 的 201（可用 wait_reply(seq)）

while True:
    d = c.inbox_deliver.get()
    print(
        "deliver from user", d.src_username,
        "conn", d.src_conn_id, "->", d.dst_conn_id,
        "bytes=", len(d.payload),
    )
```

#### 发送端

```python
import sys

sys.path.insert(0, "tools")

from relay_client import RelayClient

c = RelayClient("127.0.0.1", 19000, hb_interval_s=5)
c.connect()
c.login("alice", "alice-secret", peer_role="user", register=True)

# 发往用户名为 bob 的指定连接（conn_id 从对端 LOGIN 回复或 list_users 可得）；dst_conn_id=0 为自动选择
seq = c.send_unicast("bob", b"hello", dst_conn_id=0)
reply = c.wait_reply(seq, timeout_s=3)
print("ack:", reply)
```

### 关于「端到端已处理 ACK」

当前服务端只提供「服务器已受理」的 **201 ACK**（`op:"DATA"` 等）。若需要端到端确认，建议在 `payload` 内带 `request_id`，由对端业务回传，客户端自行匹配。

### 组帧参考

底层组帧/收包与常量见 **`local/tools/forwarder_wire.py`**（与 C++ `pack_wire` / 头布局一致）。

## C++（交付用）

- **主用（推荐）**：`deliver/client/include/fwd/asio_forwarder_client.hpp` + `deliver/client/src/asio_forwarder_client.cpp` —— 连接配置、`open` / `sign_on` / `heartbeat` / `send` / `recv_deliver`、管理员 `control_*`；内部用 `RelayClient` 实现线协议，业务一般只包含本头。
- **线协议层**（同库，高级用途）：`deliver/client/include/fwd/relay_client.hpp` + `deliver/client/src/relay_client.cpp`。
- CMake target：`asio_forwarder_sdk`；多进程示例见 `local/examples/realistic_scenario_cpp/`，C++ 黑盒回归见 `local/tests/cpp_smoke.cpp`（`build/forwarder_cpp_smoke`）。
