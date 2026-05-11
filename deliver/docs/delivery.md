# 交付说明

> **源码树**：`deliver/`。构建产物在仓库根 `build/`。  
> **`local/`**（回归脚本、网页、样例）**不在**客户最小交付包内；开发者自参见仓库 [local/README.md](../../local/README.md)。

---

## 1. 目录与产物

| 路径 | 说明 |
|------|------|
| `deliver/server/src/main.cpp` | **唯一服务端源文件**（含中继逻辑与 MySQL 访问实现） |
| `deliver/server/include/mysql_store.hpp` | MySQL 封装类声明 |
| `deliver/server/forwarder.json` | 配置模板 |
| `deliver/server/schema.sql` | 库表：`ip_allowlist`、`users` |
| `deliver/client/include/fwd/` | **对外头文件**（协议、配置、**forwarder_client.hpp** 等） |
| `deliver/client/src/forwarder_sdk.cpp` | **唯一客户端库源文件**（同步 Relay + 高层 Client） |
| `build/asio_forwarder` | 服务端可执行文件 |
| `build/libasio_forwarder_sdk.a` | 客户端静态库 |
| `build/test_admin` | 管理员能力联调可执行（`local/tests/test_admin.cpp`：CONTROL、`list_users`、`kick_user` 等） |

---

## 2. 配置文件主要键（JSON）

| 键 | 含义 |
|----|------|
| `client.listen` | 业务 TCP：`host` / `port` / `backlog` |
| `admin.listen` | 只读 HTTP：`host` / `port`（常见本机） |
| `mysql` | `host` `port` `user` `password` `database` |
| `threads.io` | `io_context` 线程数 |
| `timeouts` | `read_ms` / `idle_ms` |
| `limits.max_body_len` | 单帧正文最大长度 |
| `flow.send_queue` | 发送队列高水位与策略 `drop` \| `disconnect` |
| `session` | `max_connections_per_user`、`heartbeat_timeout_ms`、`broadcast_max_recipients` |

以下 **2.1～2.6** 用 **`deliver/server/forwarder.json` 模板** 里的数举例（你方现场若改过配置，以实际文件为准）。作用是帮人理解「这软件跑起来会怎么管人、管连接」，细节仍以 `main.cpp` 与 [protocol.md](protocol.md) 为准。

### 2.1 接入与安全

谁先算「能连上」、谁才算「能登录」、账号是不是管理员——都在这一层。

- **IP 白名单**：协议连上 TCP 之后，服务端仍要认你的**源 IP**；只有库表 `ip_allowlist` 里有的地址（字符串匹配）才接着走登录。白名单过不了，后面账号再对也没用。
- **账号表 `users`**：白名单过了才验用户名、口令；**当前实现是明文口令比对**（生产是否改摘要由你方决定）。
- **新用户名**：实现上可能「库里没有这个名字就**自动插入**再登录」，但**照样要过白名单**。交付方要清楚这是方便联调，还是你要在运维上关掉这类行为。
- **管理员**：`users.is_admin` 为真才是管理员；只有管理员能在业务口发 **CONTROL**（踢人等）。普通用户不能当管理命令用。

**现有配置**：

- `mysql`：**`host`** `127.0.0.1`，**`port`** `3306`，**`user`** `root`，**`password`** `e2etest`，**`database`** `forwarder_e2e`——库名、账号是你方环境决定；模板只是示例。
- `client.listen`：**`host`** `0.0.0.0`，**`port`** `19000`，**`backlog`** `1024`——业务 TCP 从哪进、accept 队列多长。
- `admin.listen`：**`host`** `127.0.0.1`，**`port`** `19003`，**`backlog`** `128`——只读 HTTP 管理口从哪进（一般仅供本机或内网运维；和业务口不是同一个端口）。

### 2.2 连接与会话

一条 TCP 对应一个会话；同一个登录名可以有多条 TCP，但有上限；登录成功之后才允许发业务帧。

- **单用户连接数**：限制同一个账号**同时**能挂几条到中继的 TCP，避免一端失控把连接数撑爆。
- **登录门槛**：没 LOGIN 成功前，服务端只认登录帧；避免「只占着 TCP 不报名字」的半吊子连接长期占位（具体时间限制见 **2.4 超时**）。
- **会话状态**：登录成功后，用户名、是否管理员、`recv_mode` 等会绑在这条连接上，后面发 DATA、收投递都依赖这份会话。

**现有配置**：

- `session.max_connections_per_user`：**`8`**——同一用户名最多允许多少条**仍算在线**的业务连接；超过时服务端会按实现通知并拒绝/清理（日志里可能看到与「每用户连接数」相关的 KICK/关连）。

### 2.3 转发机制

中继**不解析**你们业务报文里的「函数名、指令字」；只认**「这一包要发给哪个登录名」**，再按对方**接收策略**选一条或多条连接把二进制原样扔过去。

- **按用户名投递**：发 DATA 时带目标**登录名**；对端当前有在线连接才会真的收到（否则这包对发送方来说等于没投递到在线连接，具体错误/应答见协议）。
- **RecvMode**：对方登录时选的策略——**广播**时同一用户的**每条**在线连接都可能收到一份；**轮询**时在对方多条连接间轮流选一个投（适合同一用户多设备、只要一条收）。
- **广播封顶**：同一用户下若同时在线连接特别多，广播不能无限复制，否则内存和带宽会炸，所以要有一个**上限**。

**现有配置**：

- `session.broadcast_max_recipients`：**`10000`**——单次广播投递最多覆盖多少条**对端连接**；再多会截断并打告警日志（详见实现）。

（登录时 `recv_mode` 选 **broadcast** 还是 **round_robin**，是客户端/SDK 在 LOGIN 里带的字符串，模板不写死，但行为受上面策略约束。）

### 2.4 超时机制

防止读卡死、连接死扛不占资源、登录后长时间「装死」——用几档时间限制自动关连接或踢人。

- **单次读等待**：每一次从 socket **读固定长度**（先帧头再正文）时，等太久就放弃，避免线程永远堵在一条坏连接上。
- **整帧空闲**：很久都收不齐「完整一帧」，认为这条线废掉了，关掉。
- **登录后心跳**：登录成功后，若太久没有「算你还在」的往来（心跳、`send` 发的 DATA 等），服务端发 **KICK** 再关——调试时断点停久了常见踩坑。
- **管理口读 HTTP**：管理口上是**另一条**超时，只约束「这一次 HTTP 请求读多久」，和业务 TCP 无关。

**现有配置（业务 TCP）**：

| 配置项 | 模板取值 | 作用（人话） |
|--------|----------|--------------|
| `timeouts.read_ms` | **120000 ms（2 分钟）** | 每一小段读操作最多等这么久；**不是**「从连上到必须登录总共只能这么久」。 |
| `timeouts.idle_ms` | **600000 ms（10 分钟）** | 很久收不齐一整帧就关；若写成 **0**，当前实现相当于**关掉**这类空闲判断。 |
| `session.heartbeat_timeout_ms` | **30000 ms（30 秒）** | **仅登录成功后**：太久没有刷新活跃的帧就踢。 |

**实现细节（不配在 JSON）**：服务端大约 **每 1 秒** 扫一遍要不要因心跳超时踢人。

**管理口（代码写死）**：读一次 HTTP 请求大约 **10 秒** 超时。

**没有的配置**：模板里**没有**「TCP 建连后 N 秒内必须 LOGIN」这一项；若要，要另做开发。

### 2.5 资源保护

单帧不能无限大；往慢客户端 **写队列** 堆太高时要么丢、要么断，防止服务端被拖死。

- **单帧上限**：一帧头里声明的 body 超过阈值就按协议/实现拒绝或断会话，避免 OOM 或被恶意撑爆。
- **发送队列**：每个连接往内核/对端写之前会排队；队列字节数超「高水位」时，要么**丢**后续帧，要么**断开**连接（由策略字符串决定）；超过**硬上限**会更严厉处理。

**现有配置**：

- `limits.max_body_len`：**`67108864`**（**64 MiB**，按字节）——单帧正文允许的上限。
- `flow.send_queue.high_water_bytes`：**`67108864`**
- `flow.send_queue.hard_limit_bytes`：**`268435456`**（**256 MiB**）
- `flow.send_queue.on_high_water`：**`drop`**——高水位时先**丢**，不首选**disconnect**（若改成 `disconnect` 则偏向直接踢）。

### 2.6 运维

人用浏览器或脚本看「还活着吗、多少人连着、最近有啥事」；进程自己也会周期性打点日志——都和「业务转发」无直接关系，但排障离不开。

- **只读 HTTP**：`/api/health`、`/api/stats`、`/api/users`、`/api/events` 等给监控用（详见本文 **§5**）；**不写业务数据**。
- **事件环形缓冲**：最近若干条事件放在内存里给 `/api/events`，**不是**持久审计。
- **metrics 日志**：按间隔打一条汇总类日志给日志系统抓。
- **线程数**：IO/业务线程怎么用 CPU，粗调并发与延迟预期。

**现有配置**：

- `admin.events_max`：**`200`**——内存里保留多少条近期事件给管理口翻。
- `metrics.interval_ms`：**`3600000` ms（**1 小时**）——打 metrics 日志的节奏。
- `threads.io`：**`2`**，`threads.biz`：**`2`**——`io_context` 与业务侧线程数（含义以代码为准；调大前建议压测）。

（**管理口** 单次读超时约 **10 秒** 见 **2.4**，此处不重复展开。）

### 2.7 Web 运维页、MySQL 两张表、与业务口「管理员 CONTROL」

**`local/tools/webui_server.py` + 单页**：浏览器左侧监控里，一部分是 **GET** 代理到 C++ 的只读管理口（`/api/health` 等），另一部分是 **FastAPI 进程自己用 pymysql 连库**，对自己暴露了 `/api/ops/whitelist`、`/api/ops/accounts` 等 **增删改查**——**这些事没有走 `asio_forwarder` 进程**，也不校验你是不是业务里的 `is_admin` 用户，靠的是 **你给 Web 服务配的 MySQL 账号** 以及网络隔离（本应只在内网开）。

**业务 TCP 上的管理员账号**（`users.is_admin`）：除 **`list_users` / `kick_user`** 外，还可通过 CONTROL 对 **`ip_allowlist`**、**`users`** 做 **增删改查**（`action` 形如 `allowlist_*`、`user_table_*`，详见 **§8.1** 与 **`local/tests/test_admin.cpp`**）。权限与口令以库里为准；删除用户会先踢掉其在线会话。

**真实交付场景**仍可几种拆法（按安全需求选）：

1. **运维侧直连库**：DBA 或内部堡垒机用 SQL 客户端 / 自研运维台改表。  
2. **单独「配置服务」**：有登录鉴权，改表经 API，与中继分离（Web 页可演化成它，生产需补认证、审计、HTTPS）。  
3. **本仓库现状**：改表可走 **业务 CONTROL**（管理员）或 **Python Web 直连库**；**只读 HTTP 管理口**仍不暴露写表 API。

---

完整定义见 `deliver/client/include/fwd/config.hpp`。

---

## 3. 数据库表与职责

| 表 | 作用 |
|----|------|
| `ip_allowlist` | 允许发起 TCP 的**源 IP**（字符串全匹配，常含 `127.0.0.1`） |
| `users` | `username` / `password`（当前为明文，生产请改摘要） / `is_admin` |

登录：**先**白名单 **再** 账号；若用户名不存在则**自动插入**后登录（仍须过白名单）。

- **LOGIN 里的 `peer_role`**：须为 `user` 或 `admin`，且与库中 **`users.is_admin`** 一致（管理员账号必须用 `admin`，否则返回「登录所选权限与账号不一致」）。
- **`recv_mode`（Broadcast / RoundRobin）**：同一 **`username` 在该中继进程内「首次成功登录」**时选定后，后续再登录的同名连接沿用该策略（种子用户见 `local/tests/seed_e2e.sql`，如 `e2e_bob`、`rr2_*` 等）。

---

## 4. 客户端 API（C++，集成方）

**包含**：`#include "fwd/forwarder_client.hpp"`  
**链接**：`libasio_forwarder_sdk.a`，并链接 `pthread`、`boost_system`、`msgpack-cxx`（与 CMake target `asio_forwarder_sdk` 一致）。

命名空间 **`fwd::asio_forwarder_client`**：

| 类型 / 函数 | 作用 |
|-------------|------|
| `Client` | 一条 TCP：**open** → **sign_on**（阻塞直到 LOGIN 成功或异常）→ **send** / **recv_deliver** / **heartbeat** / **control_***；默认在 **sign_on** 成功后由后台线程每 **20s** 发送 HEARTBEAT（可用 **`set_auto_heartbeat`** / **`set_auto_heartbeat_interval`** 在 **sign_on** 前关闭或改间隔） |
| `Client::set_auto_heartbeat` / `set_auto_heartbeat_interval` | 须在 **sign_on** 前调用；后台仅发出 HEARTBEAT 帧（**不**读 socket），与业务线程并发写路径已加锁；显式 **`heartbeat()`** 仍可用 |
| `Client::ConnectionConfig` | `host` + `port` |
| `RecvMode` | `Broadcast` / `RoundRobin`（与登录时字符串 `broadcast` / `round_robin` 对应） |
| `Client::send(target_username, payload_bytes, SendOptions)` | 发 DATA；默认 **`wait_server_accept=true`** 会在发完后读掉对应 **201**；若设为 `false`，须自行保证 **201** 在后续 **`recv_deliver`** 之前被消费，否则会误把 201 当投递读入 |
| `Client::recv_deliver()` | 阻塞收一条 200 投递 |
| `Client::control_list_users`（`wait==true` 时返回 201 正文 `optional<object_handle>`，含 `users`）/ `control_kick_user` | 管理命令（需管理员账号） |
| `Client::control_request` | 发任意 CONTROL msgpack map，收 201 解析用（库表 CRUD 等） |
| `try_login(...)` | 仅探测登录是否成功 |
| `LocalForwarder` | **Linux 联调用**：fork 子进程跑 `asio_forwarder` 临时配置（可选） |
| `admin_health_ok(host, admin_port)` | GET `/api/health` 探测 |

底层同步套接字实现类 **`fwd::sdk::RelayClient`**（头文件 `relay_client.hpp`）：`connect` / `login` / `send_data` / `recv` 等；一般业务优先用上层 `Client`。

---

## 5. 只读管理口 HTTP

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health`、`/health` | 存活 |
| GET | `/api/stats` | 连接数、帧/字节计数等 |
| GET | `/api/users` | 在线用户、`recv_mode`、连接编号与对端地址、`is_admin` |
| GET | `/api/events` | 最近事件环形缓冲 |
| OPTIONS | `*` | CORS 预检 |

---

## 6. 可选交付物

- **`local/tools/`**：Python 线缆工具、**Web 单页**（监控 + 模拟终端）等。  
- **联调可执行**：`local/tests/test_admin.cpp` → **`test_admin`**（管理员 CONTROL、`list_users` / `kick_user` 演示）。

---

## 7. 协议与性能

- 线协议全文：**[protocol.md](protocol.md)**  
- 性能测试与填表：**[performance.md](performance.md)**

---

## 8. 能力与操作一览（拿到软件后你能「做」什么）

**可以同时用**：本机浏览器打开 `local/tools/webui_server.py` 提供的页面做联调，业务代码用 C++ SDK 连**同一业务端口**——二者互不排斥，只要 MySQL 里账号与白名单允许你的来源 IP。

### 8.1 服务端进程（`asio_forwarder` + 配置文件）

| 能力 | 说明 |
|------|------|
| 监听业务 TCP | `client.listen`：客户端 LOGIN / HEARTBEAT / DATA / CONTROL 均走此端口 |
| 鉴权 | 先查 `ip_allowlist`，再查 `users`；新用户名可自动插入后登录（仍须过白名单） |
| 按登录名路由 DATA | 不解析业务语义，只按**目标用户名**与目标当前 `RecvMode` 投递 |
| 只读 HTTP 管理口 | `admin.listen`：`/api/health`、`/api/stats`、`/api/users`、`/api/events` 等（见第 5 节） |
| 踢线、管理 CONTROL | 由**管理员账号**在业务 TCP 上发 CONTROL（在线 `list_users` / `kick_user`；**表** `allowlist_list|add|update|delete`，`user_table_list|add|update|delete`）；观测结果也可在 `/api/users` 看到 |

服务端**不提供**在二进制协议里「列出可执行函数表」——可操作项即：各 JSON 配置项 + 上述 HTTP 路径 + 线协议规定的帧类型（见 protocol.md）。

### 8.2 C++ 客户端库（`libasio_forwarder_sdk.a` + `fwd/forwarder_client.hpp`）

| API / 对象 | 典型用途 |
|------------|----------|
| `Client::open` | 建立到中继的 TCP |
| `Client::sign_on` | LOGIN，选 `RecvMode`（Broadcast / RoundRobin）与 `peer_role` |
| `Client::local_username` | 确认当前登录名 |
| `Client::send` | 向某**登录名**发二进制 DATA；`SendOptions::wait_server_accept` 是否等待 201 |
| `Client::send_poly` / `send_typed` | 在载荷外再包 msgpack（kind/type/data 或 type/data），仍走 DATA 路由 |
| `Client::recv_deliver` | 阻塞收一条发往本连接的投递（200） |
| `Client::heartbeat` | 发 HEARTBEAT，配合服务端超时配置 |
| `Client::control_list_users` / `control_kick_user` | 管理员：列出在线用户与连接（201 正文）、按 user id 踢 |
| `Client::control_request` | 发任意 CONTROL msgpack map（如库表 CRUD），收 201 后解析正文 |
| `Client::raw` | 访问 `sdk::RelayClient`：**connect/login/send_data/recv** 等更低层 |
| `try_login` | 短连接探测账号是否可登录 |
| `admin_health_ok` | HTTP GET 探测管理口 |
| `LocalForwarder` | （Linux）fork 子进程起临时中继，多用于自测 |

管理员与库表 CONTROL 的用法示例见 **`local/tests/test_admin.cpp`**（`test_admin` 可执行文件）。
