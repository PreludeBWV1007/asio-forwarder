# asio-forwarder

基于**C++20**和**Boost.Asio** 实现的 **转发器**，实现多接入的二进制消息转发功能。

工作流程：先新建连接，多客户端连同一**业务端口**；通过 **MySQL** 校验 **IP 白名单**与**账号**；登录后按**目标登录名**互发**二进制载荷**（中继不解析内容）。

## 交付形态

| 角色 | 交付物 |
|------|--------|
| 服务端 | 可执行文件 **`asio_forwarder`** + JSON **配置文件** + `schema.sql` |
| 客户端 | 头文件目录 **`deliver/client/include/fwd/`** + 静态库 **`libasio_forwarder_sdk.a`**（实现为**单个** `forwarder_sdk.cpp`） |
| 说明 | 本仓库仅保留四份说明：**本文**、`deliver/docs/protocol.md`（线协议）、`deliver/docs/delivery.md`（交付与 API 表）、`deliver/docs/performance.md`（性能与测试） |

集成 C++ 客户端时推荐**只包含** `fwd/forwarder_client.hpp`（对 `asio_forwarder_client` 的薄入口）。

## 构建

```bash
./scripts/build.sh
```

依赖：Boost、msgpack-cxx（CMake 自动拉取）、**libmysqlclient**（仅编译服务端）。

## 运行服务端

```bash
./build/asio_forwarder deliver/server/forwarder.json
```

本机 MySQL 若要求密码而 JSON 里 `mysql.password` 为空，可：

```bash
export FORWARDER_MYSQL_PASSWORD='你的密码'
./scripts/run_dev.sh
```

`run_dev.sh` 仍启动 `build/asio_forwarder`；可用 `FORWARDER_CONFIG` 指向自建 JSON。

## 回归与性能

- **端到端**（需本机 MySQL，脚本会建库 `forwarder_e2e` 并导入 `schema.sql` + `local/tests/seed_e2e.sql`）：

  ```bash
  cd build && ctest --output-on-failure
  # 或
  ./local/tests/run_e2e.sh
  ```

  通过时最后一行：`---- OK: e2e passed ----`。黑盒脚本已合并为 **`local/tests/e2e_forwarder.py`**。

- **基本性能**（需已有中继与种子用户 `perf_src` / `perf_dst`）：

  ```bash
  ./build/forwarder_perf 127.0.0.1 业务端口 2000
  ```

  将终端输出填入 `deliver/docs/performance.md` 中表格。详见该文件。

## 本机网页（非必交付）

`local/tools/webui_server.py`：单页监控 + 浏览器侧 WebSocket 模拟终端；仅供联调。**刷新页面会丢失浏览器里的会话列表**——这是正常现象：真实连接在内存里，不落库；生产不使用该页则无需持久化「连接表」。

与 C++ 可同时用：页面与业务代码都连**同一业务端口**，前提是 MySQL 白名单与账号允许。**第一次写 C++ 集成**可看带注释的示例：[`local/examples/usage_instruction.cpp`](local/examples/usage_instruction.cpp)（构建后 `./build/usage_instruction HOST BIZ_PORT [ADMIN_HTTP_PORT]`）；服务端与客户端「都能做什么」总表见 [`deliver/docs/delivery.md`](deliver/docs/delivery.md) 第 8 节。

## 文档索引

| 文档 | 内容 |
|------|------|
| [deliver/docs/protocol.md](deliver/docs/protocol.md) | 帧结构、登录/数据/管理、只读管理口 |
| [deliver/docs/delivery.md](deliver/docs/delivery.md) | 目录、配置键、客户端 API 表、数据库表 |
| [deliver/docs/performance.md](deliver/docs/performance.md) | 测试与性能记录方式 |
| [local/README.md](local/README.md) | 仓库内 `local/` 脚本说明（非客户最小包） |
