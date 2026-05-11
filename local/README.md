# 本地 `local/`（非客户最小包）

研发用：**回归**、Python 工具、**单页 Web**、C++/Python 样例。需已构建 `build/asio_forwarder` 与 `build/libasio_forwarder_sdk.a`。

```
local/
├── tests/       # test_admin.cpp、test_user.cpp、seed_e2e.sql
└── tools/       # forwarder_wire、relay_client（webui 等）、webui_server、app.html …
```

- **联调**：`./build/test_admin …`（管理员）；`./build/test_user …`（普通用户收发/权限探测）。均依赖已导入 `tests/seed_e2e.sql` 的同库中继。
- **Web**：`PYTHONPATH=local/tools python3 local/tools/webui_server.py` → `http://127.0.0.1:8080`。刷新页面会清空浏览器侧会话；真实 TCP 由中继进程持有，与是否落库无关。

文档以仓库根 **README.md** 与 **deliver/docs/** 下三文件为准。
