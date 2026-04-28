# 本地 `local/`（非客户最小包）

研发用：**回归**、Python 工具、**单页 Web**、C++/Python 样例。需已构建 `build/asio_forwarder` 与 `build/libasio_forwarder_sdk.a`。

```
local/
├── tests/       # run_e2e.sh、e2e_forwarder.py、perf_basic.cpp、seed_e2e.sql
├── tools/       # forwarder_wire、relay_client（e2e/webui 依赖）、webui_server、app.html …
└── examples/    # first_use（注释入门 C++）
```

- **E2E**：`./local/tests/run_e2e.sh` 或 `cd build && ctest`。  
- **性能**：`./build/forwarder_perf HOST PORT N`，结果写入 `deliver/docs/performance.md`。  
- **Web**：`PYTHONPATH=local/tools python3 local/tools/webui_server.py` → `http://127.0.0.1:8080`。刷新页面会清空浏览器侧会话；真实 TCP 由中继进程持有，与是否落库无关。

文档以仓库根 **README.md** 与 **deliver/docs/** 下三文件为准。
