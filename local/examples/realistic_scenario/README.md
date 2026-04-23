# 示例：模拟真实业务使用中继

本目录用于展示“把中继当黑盒”的真实用法：假设有一个**任务分发系统**：

- **dispatcher（调度者）**：把任务发给某个 worker（按用户名）
- **worker（执行者）**：处理任务后回传结果给 dispatcher
- **admin（管理员）**：观测在线用户、必要时踢人（带原因）

运行前准备：

```bash
pip install -r local/tools/requirements-relay.txt
export PYTHONPATH=local/tools
```

启动中继（新终端）：

```bash
./build/asio_forwarder deliver/server/forwarder.json
```

然后分别启动三个进程（不同终端）：

```bash
python3 local/examples/realistic_scenario/dispatcher.py --host 127.0.0.1 --port 19000
python3 local/examples/realistic_scenario/worker.py --host 127.0.0.1 --port 19000 --username worker1
python3 local/examples/realistic_scenario/admin.py --host 127.0.0.1 --port 19000
```

说明：

- 三个脚本都会在第一次运行时 `register=True` 注册账号；后续改成 `register=False` 即登录。
- dispatcher 使用 `send_unicast(dst_username=...)` 发任务；worker 收到 DELIVER 后解析 payload（这里用 JSON 文本模拟业务）并回传结果。
- admin 使用 `CONTROL list_users/kick_user`（需管理员账号 `peer_role=admin`）。

