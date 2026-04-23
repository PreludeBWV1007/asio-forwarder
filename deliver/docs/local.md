# 本地开发 / 测试

本页描述仓库中 **`local/`** 目录：回归脚本、联调工具、示例；**使用它们时依赖已构建的 `build/asio_forwarder` 与 `deliver/client/include` 头文件**（由根 `CMakeLists.txt` 管理）。

**交付物**清单见 [delivery.md](delivery.md)；**交付**与**本地**关系见根目录 [README.md](../../README.md)。

---

## 一、`local/tests/`（统一命名）

| 文件 | 作用 |
|------|------|
| `local/tests/run_e2e.sh` | 总入口：`ctest` 的 `e2e_forwarder` 调用之；起临时服 → `e2e_minimal` → `e2e_suite` → 可选 `forwarder_cpp_smoke` → `admin` health |
| `local/tests/e2e_minimal.py` | 最短 Python 黑盒 |
| `local/tests/e2e_suite.py` | 多场景 Python 黑盒 |
| `local/tests/cpp_smoke.cpp` | C++ SDK 黑盒 → **`build/forwarder_cpp_smoke`** |

> Python 需 `export PYTHONPATH=local/tools`（`forwarder_wire` 在 `local/tools/forwarder_wire.py`），与 **`deliver/server/src/main.cpp`** 行为、**`deliver/client/include/fwd/protocol.hpp`** 对齐。

`scripts/test_e2e.sh` 仅 **exec** 到 `local/tests/run_e2e.sh`。

---

## 二、怎么跑

```bash
./scripts/build.sh
cd build && ctest --output-on-failure
# 或
./local/tests/run_e2e.sh
```

单跑 Python 黑盒（需中继已启动）：

```bash
export PYTHONPATH=local/tools
python3 local/tests/e2e_minimal.py 127.0.0.1 19000
```

单跑 C++ smoke：`./build/forwarder_cpp_smoke`（见 `--help`）

---

## 三、`local/tools/` 联调

- `relay_cli.py`：`--config` 默认 **`deliver/server/forwarder.json`**
- `relay_client.py`、`run_scenario.py`、`webui_server.py` + `webui_static/`

## Web 演示

```bash
pip install -r local/tools/requirements-relay.txt -r local/tools/requirements-webui.txt
export PYTHONPATH=local/tools
python3 local/tools/webui_server.py --relay-host 127.0.0.1 --relay-port 19000
```

**无**浏览器自动化用例；交付时可选不打包。

**压测与 `flood`：**交互式 **`local/tools/relay_cli.py`** 中 `flood` 的 RTT 定义为“发出带 `seq` 的帧 → 收到对应 **201** 的往返时间”。具体命令见该工具内 `help`；**数值随 CPU/loopback/客户端语言而变**，仅作本机相对参考，勿当跨环境 SLA。

## 四、`local/examples/`

- `realistic_scenario_cpp/`：多进程 C++ 示例；`local/tests/cpp_smoke.cpp` 使用其中 `messages.hpp`。
- `realistic_scenario/`：旧 Python 三进程示例。

## 回归记录（可更新）

| 日期 | 步骤 | 结果（摘要） |
|------|------|----------------|
| 2026-04-23 | 目录重排前 `ctest` | `e2e_forwarder` **Passed**（约 122 s，路径以当时脚本为准） |

## 与实现的对应

- 业务逻辑：`deliver/server/src/main.cpp` 中 `RelayServer::handle_client_frame`
- 配置：`deliver/client/include/fwd/config.hpp`、`deliver/server/forwarder.json`
- 协议： [protocol.md](protocol.md)
