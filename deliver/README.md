# 交付目录 `deliver/`

| 子目录 | 内容 |
|--------|------|
| `server/` | 单源文件服务端 + 配置模板 + `schema.sql` |
| `client/include/fwd/` | 协议与客户端 API 头文件；集成入口 **`forwarder_client.hpp`** |
| `client/src/` | **`forwarder_sdk.cpp`**（唯一库实现） |
| `docs/` | **[protocol.md](docs/protocol.md)**、**[delivery.md](docs/delivery.md)**、**[performance.md](docs/performance.md)** |

总览与构建步骤见仓库根 **[README.md](../README.md)**。
