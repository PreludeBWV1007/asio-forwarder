# 交付（发布给集成方/运维的内容）

> **源码树位置**：`deliver/`。物理对应关系见 [../README.md](../README.md) 与 [../../README.md](../../README.md)。  
> **本地测试**（`local/`）不在最小交付内，见 [local.md](local.md)。

---

## 总览

| 块 | 内容 |
|----|------|
| **服务器** | `deliver/server/`：源码、配置模板、协议入口（`protocol.md` 链接） |
| **客户端** | `deliver/client/include/fwd/` + `deliver/client/src/*.cpp` → 产出 `libasio_forwarder_sdk.a` |
| **说明文档** | 本 `deliver/docs/` 目录 |
| **辅助 Web** | 在 **`local/tools/`**（`webui_server.py`），可选交付 |

---

## 1. 服务器

| 交付物 | 路径 |
|--------|------|
| 可执行文件（构建后） | `build/asio_forwarder`（源：`deliver/server/src/main.cpp`） |
| 配置模板 | `deliver/server/forwarder.json` |
| 协议（可与二进制同包） | `deliver/docs/protocol.md` 或 `deliver/server/protocol.md`（符号链接到前者） |

```bash
./build/asio_forwarder /path/to/your.json
```

（开发机可用 **`deliver/server/forwarder.json`** 或 `scripts/run_dev.sh`。）

---

## 2. 客户端（C++ SDK）

| 项 | 路径 |
|----|------|
| 头文件（整包 `fwd/`，随库交付） | `deliver/client/include/fwd/` |
| 实现 | `deliver/client/src/relay_client.cpp`、`asio_forwarder_client.cpp` |
| CMake target | `asio_forwarder_sdk`（根 `CMakeLists.txt`） |

多进程大示例在 **`../../local/examples/realistic_scenario_cpp/`**（不属 `deliver/` 最小编，但常一并交付作演示）。

---

## 3. 说明文档

本目录下 **[protocol.md](protocol.md)**、**[client_api.md](client_api.md)**、**[architecture.md](architecture.md)** 等。

---

## 4. 辅助 Web（可选）

组件位于 **`local/tools/`**；安装与启动见 [local.md](local.md)「Web」。

---

## 建议最小包（参考）

- **仅 C++**：`asio_forwarder` + 环境用 json + `deliver/docs/protocol.md` + `deliver/client/include` + 静态库 + 可选 `local/examples/realistic_scenario_cpp/`。  
- **含研发工具**：再附加整个 **`local/`**（Python 与 `local/tests/` 等）。
