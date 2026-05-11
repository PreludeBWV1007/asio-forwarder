# asio-forwarder

基于**C++20**和**Boost.Asio** 实现的 **转发器**，实现多接入的二进制消息转发功能。

## 使用步骤

### 环境准备
- `基础依赖`：CMake、C++20编译器、Boost、libmysqlclient、Python3
- `Mysql`：启动，且能用TCP连上（127.0.0.1：3306）。默认配置里库名是 forwarder_e2e，用户 root，密码示例 e2etest（见 deliver/server/forwarder.json）。若你本机不同，后面启动时要用 FORWARDER_MYSQL_PASSWORD 或改自己的 JSON。
```bash
./scripts/build.sh
```
成功后会有 `build/asio_forwarder`、`build/test_admin`、`build/test_user` 及 SDK 静态库等。

- `准备数据库`：在能连 MySQL 的前提下执行：
```bash
export MYSQL_PWD='e2etest'   # 若 root 无密码可省略或设空
mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp -e "CREATE DATABASE IF NOT EXISTS forwarder_e2e"
mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp forwarder_e2e < deliver/server/schema.sql
mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp forwarder_e2e < local/tests/seed_e2e.sql
```
seed_e2e.sql 里有 e2e 账号、白名单等。

### 服务端
- `业务TCP端口`：19000
- `管理HTTP`：127.0.0.1:19003
- `配置`：deliver/server/forwarder.json。需要保证mysql密码更新到JSON中最新。

```bash
./build/asio_forwarder deliver/server/forwarder.json
```

### 客户端
- `基础`：持有：头文件目录 **`deliver/client/include/fwd/`** + 静态库 **`libasio_forwarder_sdk.a`**
- `使用`：功能汇总和实现详情见：[deliver/docs/usage.md](deliver/docs/usage.md)

运行集成小测（需已按上文导入 `local/tests/seed_e2e.sql` 且中继已启动）：
```bash
./build/test_admin 127.0.0.1 19000   # 管理员 CONTROL / 踢人 / 两表 CRUD
./build/test_user 127.0.0.1 19000     # 普通用户 DATA 往返 / try_login / 非管理员禁止 CONTROL
```

### 本地Web端
本地Web端更多服务于测试和调试，需要在同一台主机下，同时有服务端和客户端的运行基础，本地开设不同端口来模拟多客户端的多连接。**刷新页面会丢失浏览器里的会话列表**——这是正常现象：真实连接在内存里，不落库。

- `安装Web依赖`：
```bash
pip install -r local/tools/requirements-relay.txt -r local/tools/requirements-webui.txt pymysql
```
- `启动Web UI`：
```bash
PYTHONPATH=local/tools python3 local/tools/webui_server.py
```
- `浏览器打开`：http://127.0.0.1:8080

### 性能测试

仓库内已不再附带自动化 E2E 脚本与 `forwarder_perf`；可自行编写客户端测往返或更新 `deliver/docs/performance.md` 中的记录方式。管理员能力联调见上一节 `test_admin`。

### 支持文档

| 文档 | 内容 |
|------|------|
| [deliver/docs/protocol.md](deliver/docs/protocol.md) | 帧结构、登录/数据/管理、只读管理口 |
| [deliver/docs/delivery.md](deliver/docs/delivery.md) | 目录、配置键、客户端 API 表、数据库表 |
| [deliver/docs/performance.md](deliver/docs/performance.md) | 测试与性能记录方式 |
| [local/README.md](local/README.md) | 仓库内 `local/` 脚本说明（非客户最小包） |

### 交付形态

| 角色 | 交付物 |
|------|--------|
| 服务端 | 可执行文件 **`asio_forwarder`** + JSON **配置文件** + `schema.sql` |
| 客户端 | 头文件目录 **`deliver/client/include/fwd/`** + 静态库 **`libasio_forwarder_sdk.a`**（实现为**单个** `forwarder_sdk.cpp`） |
| 说明 | 本仓库仅保留四份说明：**本文**、`deliver/docs/protocol.md`（线协议）、`deliver/docs/delivery.md`（交付与 API 表）、`deliver/docs/performance.md`（性能与测试） |

集成 C++ 客户端时推荐**只包含** `fwd/forwarder_client.hpp`（对 `asio_forwarder_client` 的薄入口）。