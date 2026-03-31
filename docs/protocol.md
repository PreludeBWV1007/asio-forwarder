# 协议（第一版，稳定性优先）

## 字节序

- 统一使用 **little-endian**（小端）。

## Frame

一个消息帧 = `Header(24 bytes)` + `Body(body_len bytes)`。

### Header（24 bytes）

| 字段 | 类型 | 说明 |
|---|---:|---|
| magic | u32 | 固定 `0x44574641`（用于快速识别协议） |
| version | u16 | 固定 `1` |
| header_len | u16 | 固定 `24` |
| body_len | u32 | body 长度，必须 `<= limits.max_body_len` |
| msg_type | u32 | 业务消息类型（预留） |
| flags | u32 | 标志位（预留） |
| seq | u32 | 序列号（预留） |

### Body

- 原样转发的二进制载荷（未来可改为 MsgPack/Protobuf/JSON 序列化后的 bytes）。

