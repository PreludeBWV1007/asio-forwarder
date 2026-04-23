# 本地（`local/`）

研发自测与联调：**不单独构成客户最小交付包**；使用本目录下的脚本时，**依赖**仓库里已构建好的 `build/asio_forwarder`、以及 `deliver/client/include` 的头文件（由根目录 `CMakeLists.txt` 一并编进 `libasio_forwarder_sdk.a`）。

```
local/
├── tests/          # run_e2e.sh、e2e_minimal/suite、cpp_smoke.cpp
├── tools/          # relay_cli、relay_client、forwarder_wire、webui …
└── examples/       # realistic_scenario_cpp、旧 Python 示例等
```

常见入口：

- `cd build && ctest`（内部执行 `local/tests/run_e2e.sh`）
- `PYTHONPATH=local/tools python3 local/tools/relay_cli.py --spawn-server`
