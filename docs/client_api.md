# 生产交付：客户端接口（Python）

本仓库的服务器协议是：**v2 header（40B 小端）+ body**。上行 body 为 msgpack map（`op` 状态机），下行分两类：

- **200 DELIVER**：body 为 payload 原始字节
- **201 SERVER_REPLY**：body 为 msgpack（ACK/错误/CONTROL 返回）

如果你的生产系统需要“传输结构体/class 对象”，推荐做法是：在客户端把对象**序列化为 bytes**（msgpack/protobuf/flatbuffers/自定义二进制均可），作为 `TRANSFER.payload` 发送；对端收到 200 后反序列化。

## `RelayClient`（最小可用生产接口）

位置：`tools/relay_client.py`

特点：
- 一个 `RelayClient` 实例 = 一条 TCP 连接
- 提供自动心跳（可关）
- 提供 send（unicast/broadcast/round_robin）、CONTROL
- 提供接收回调或队列

### 用法示例（两端）

#### 接收端（user_id=200）

```python
import sys
sys.path.insert(0, "tools")

from relay_client import RelayClient

c = RelayClient("127.0.0.1", 19000, hb_interval_s=5)
c.connect()
seq = c.login(200, "data")
print("login sent seq=", seq)

while True:
    d = c.inbox_deliver.get()
    print("deliver from", d.src_user_id, "bytes=", len(d.payload), "payload=", d.payload)
```

#### 发送端（user_id=100）

```python
import sys
sys.path.insert(0, "tools")

from relay_client import RelayClient

c = RelayClient("127.0.0.1", 19000, hb_interval_s=5)
c.connect()
c.login(100, "data")

seq = c.send_unicast(200, b"hello")
reply = c.wait_reply(seq, timeout_s=3)
print("ack:", reply)
```

### 关于“端到端已处理 ACK”

当前服务端只提供“服务器已受理”的 **201 ACK**（`op:"TRANSFER"`）。如果你需要端到端确认（对端业务已处理），建议在 payload 内封装 `request_id` 并由对端业务回传响应 payload，客户端用 request_id 做匹配与阻塞等待。

