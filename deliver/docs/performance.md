# 性能与测试

## 自动化回归（功能）

一条命令（需本机 MySQL，`root` 口令与脚本环境变量一致）：

```bash
./scripts/build.sh
cd build && ctest --output-on-failure
```

或直接：

```bash
./local/tests/run_e2e.sh
```

脚本会：起临时配置的 `asio_forwarder` → 跑 **`local/tests/e2e_forwarder.py`**（原 minimal + suite 合并）→ 可选 **`first_use_client`**（C++ 入门闭环）→ GET 管理口 `/api/health`。成功结尾：`---- OK: e2e passed ----`。

数据库：`forwarder_e2e`；表与种子见 `deliver/server/schema.sql`、`local/tests/seed_e2e.sql`。

---

## 基本性能（闭环 RTT）

可执行文件 **`forwarder_perf`**（源码 `local/tests/perf_basic.cpp`）：两个账号 `perf_src` → `perf_dst` 各一条连接，循环「发 DATA + 等 201 + 对端收 200」。

```bash
./build/forwarder_perf 127.0.0.1 <业务端口> 2000
```

输出示例：`forwarder_perf roundtrips=2000 payload_bytes=64 total_ms=… rtt_avg_ms=…`  
数值强依赖 **CPU、loopback、是否与其它负载共享 MySQL**，仅作本机对比，不作对外 SLA。

**大额 `roundtrips` 时**：默认配置里 `session.heartbeat_timeout_ms` 为 30000 时，长跑可能被服务端判为心跳超时踢线；压测可把该值临时调高（或缩短 `N` 并在 30s 内跑完）。

---

## 结果记录

| 项目 | 值 | 备注 |
|------|-----|------|
| 测试日期 | 2026-04-28 | |
| CPU / 系统 | x86_64，32 vCPU，`lscpu` Model name: General Processors；Ubuntu 22.04；Linux 5.15 | 云虚拟机 |
| roundtrips | 2000 | |
| payload_bytes | 64 | 与 `perf_basic.cpp` 一致 |
| total_ms | 88935 | `forwarder_perf` 输出 |
| rtt_avg_ms | 44.4675 | 同上 |
| e2e | Passed | `E2E_MYSQL_PASSWORD=e2etest ./local/tests/run_e2e.sh` |

原始一行输出：

`forwarder_perf roundtrips=2000 payload_bytes=64 total_ms=88935 rtt_avg_ms=44.4675`

（本次中继配置在 `deliver/server/forwarder.json` 基础上仅将 `session.heartbeat_timeout_ms` 改为 **600000**，其余不变；业务端口 **19000**。）

---

## 说明

- **不要**把浏览器 Web 模拟终端的会话当作性能基准；应以原生 TCP 客户端或 `forwarder_perf` 为准。  
- 若需压测到极限，请另行加大 `N`、调整 `flow.send_queue` 与线程数，并观察服务端日志与 `/api/stats`。
