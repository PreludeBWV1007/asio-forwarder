# 交付包（`deliver/`）

本目录是**可直接对应客户交付物**的源码与文档树（构建产物仍在仓库根 `build/`，打包时拷贝即可）。

```
deliver/
├── server/                    # 中继服务
│   ├── src/main.cpp
│   ├── forwarder.json         # 配置模板（由旧 configs/dev 迁入）
│   ├── protocol.md            # → ../docs/protocol.md（符号链接，便于与可执行文件同目录交付）
│   └── …
├── client/                    # C++ SDK（头文件 + 实现源）
│   ├── include/fwd/           # 整目录随库交付（协议 + SDK 头）
│   └── src/                   # relay_client / asio_forwarder_client 实现
└── docs/                      # 协议、API、架构等说明（与上面配合阅读）
```

运行时逻辑关系：**`server` 与 `client` 共享同一条 `fwd/` 头文件树**（协议在头文件里；可执行文件只编 `server/src/main.cpp`）。
