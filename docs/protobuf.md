# Protobuf 与外层帧（完整约定）

## 分层

| 层次 | 内容 |
|------|------|
| 传输帧 | 固定 40 字节 `Header`（v2，见 `docs/protocol.md`）+ `Body`；转发器校验长度、`magic`/`version`/`header_len` 并**原样广播** |
| Body 语义 | `proto/market.proto` 中的 message 经 `SerializeToString` 得到的字节；**转发器不解析** |

## `WireMsgType` 与 `Header.msg_type`

在 `proto/market.proto` 中定义枚举 **`WireMsgType`**，与 **`Header.msg_type`（u32）** 使用**相同数值**，便于下游在未解码 Body 前做粗分流。

| 数值 | 枚举名 | Body 解析为 |
|-----:|--------|-------------|
| 100 | `STOCK_QUOTE` | `StockQuote` |
| 101 | `HEARTBEAT` | `Heartbeat` |
| 102 | `MARKET_PAYLOAD` | `MarketPayload`（`oneof` 内嵌 `StockQuote` 或 `Heartbeat`） |

约定：**发送端**应对齐 `msg_type` 与 Body；**接收端**以 `msg_type` 为主解析路径；`proto_recv` 在类型不匹配时会尝试启发式解析并标注 `(hint)`。

`MarketPayload` 适用于希望**只调用一次 `ParseFromString`** 再在 `oneof` 上分支的场景。

## 契约与演进

- 单文件入口：`proto/market.proto`（可继续在同一文件增加 message / enum，或拆多文件后由 CMake `proto/*.proto` 一并生成）。
- 修改字段遵守 [proto3 更新规则](https://protobuf.dev/programming-guides/proto3/#updating)：新字段用**新编号**、废弃编号 `reserved`、勿改字段语义与类型。

## C++ 构建

依赖：`libprotobuf-dev`、`protobuf-compiler`。

```bash
./scripts/build.sh
```

CMake 使用 `file(GLOB ... proto/*.proto)` + `protobuf_generate_cpp`，生成文件在 `build/`（如 `market.pb.h` / `market.pb.cc`），链入静态库 **`market_proto`**。

### 公共小工具头

- `include/fwd/frame_io.hpp`：`pack_frame`、`recv_frame`、`tcp_connect`、阻塞式 `send_all`/`recv_exact`（POSIX，无 Boost），供 `proto_send` / `proto_recv` 及业务侧参考。

### 可执行文件

| 目标 | 作用 |
|------|------|
| `build/proto_send` | `--kind stock|heartbeat|envelope` 组 Body，默认按 kind 填 `WireMsgType`；可用 `--type` 覆盖 |
| `build/proto_recv` | 连 **downstream**；`--count N`（`0` = 直到 EOF）；`--hex-on-fail` 在解析失败时打印 hex 预览 |

示例：

```bash
# 心跳
./build/proto_send --kind heartbeat --node-id gw-1
# 外层 envelope（msg_type=102）
./build/proto_send --kind envelope --inner quote --symbol 000001.SZ
# 收多帧
./build/proto_recv --count 0 --timeout 0
```

## Python

1. 生成 `*_pb2.py`：

```bash
./scripts/gen_proto_python.sh
```

2. 安装运行时（**建议用 venv**；若全局已装 `protobuf` 6.x，会与 apt 的 `protoc 3.12` 生成物不兼容）：

```bash
pip install -r tools/requirements-proto.txt
```

若 `import market_pb2` 报错 `Descriptors cannot be created directly`：当前 `requirements-proto.txt` 已限制 `protobuf<4`；或升级本机 `protoc` 至 ≥ 3.19 后重新执行 `gen_proto_python.sh`，再使用较新的 Python `protobuf` 包。

3. 发包（与 C++ 帧格式一致）：

```bash
python3 tools/upstream_send_proto.py --kind stock --symbol 600000.SH
python3 tools/upstream_send_proto.py --kind heartbeat --node-id py-1
python3 tools/upstream_send_proto.py --kind envelope --inner heartbeat
```

生成物路径：`tools/generated/market_pb2.py`（已在仓库 `.gitignore` 中忽略，避免与本地 `protoc` 版本漂移冲突）。

## 扩展新消息类型

1. 在 `proto/market.proto`（或新 `.proto`）中增加 `message` / `WireMsgType` 取值。
2. 若为新文件，放入 `proto/`，CMake 会自动纳入生成。
3. 更新 **`proto_send` / `proto_recv` / `upstream_send_proto.py`** 的分支（或改为通用分发表）。
4. 更新本文档的 **`WireMsgType` 表**。

## 与转发器主程序的关系

`asio_forwarder` **不链接** `market_proto`，行为与是否使用 Protobuf **无关**；业务上下游自行序列化/反序列化即可。
