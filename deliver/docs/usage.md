# 使用方法

汇总项目各个模块的功能、原理、方法及使用方式。

## 客户端
引入的包是：`#include "fwd/forwarder_client.hpp"`。

### 功能、原理和使用方法
- `Client类`：这是一个逻辑客户端，只是代表一个连接，即，一个 Client 实例 = 一条 TCP 连接 = 一次 sign_on 所作用的那条连接。注意，如果要同一个用户多个连接，需要建立新的Client。
- `Client类方法open`：实际实现是依赖内部的RelayClient::connect，用 Asio做解析主机名 + 建 TCP 连接（同步、阻塞直到连上或失败），新建一条从客户端到服务器的TCP连接，不包含账号登陆。该连接建立后的状态是【未登录】，首帧除LOGIN外，其他的都会被拒。
- `Client类方法sign_on`：在已建立的连接上登陆用户，携带用户名、密码、接收类型（RoundRobin/Broadcast）、用户类型（user/admin）。内部依赖RelayClient::login用msgpack打一个map，阻塞等待服务端的返回。服务端状态机解析并拆包，校验顺序是：报文完整性、IP白名单、用户账号密码（**登录只校验已有账号；新增账号只通过库初始化或管理员 user_table_add添加用户**）、用户-连接表（连接队列/多连接更新、连接数上限），应答（失败则有提示信息）。客户端依赖RelayClient::recv接收，socket读满header，再按body_len读满body。
- `Client类方法heartbeat`：向已登录连接发送 HEARTBEAT；**登录成功后默认另有后台线程每 20s 自动发送**（见 `set_auto_heartbeat` / `set_auto_heartbeat_interval`，须在 **sign_on** 前配置）。若关闭自动心跳，须在超时时间内自行调用、或依赖其它上行帧刷新服务端活跃时间。
- `Client类方法send/send_typed/send_poly`：区别体现在payload形状上。send是任意字节，适合裸协议。send_typed适合结构化数据且不区分消息族/版本/子类型，用msgpack来包结构体，payload形状为{type, data}。send_poly适合结构化数据且不区分消息族/版本/子类型，结构为{kind, type, data}。
- `Client类方法recv_deliver`：底层是read凑齐到一帧，没有内置超时，所以外部看来，此函数是阻塞等待的。若想「等一阵没数据就做别的事」，要么 另一条线程 做别的事，要么 不用阻塞 API（自写 async_read+定时器、或 poll/select 带超时），要么在别的线程 close 连接 叫醒阻塞读。到达的数据会积存在本机TCP接收缓冲区。
- `Client类方法control_list_users/control_kick_user/control_request`：`control_list_users` 发 `list_users`；`wait_server_reply==true` 时返回 `std::optional<msgpack::object_handle>`（201 正文 unpack，含 `users` 等）；`false` 仅发出请求并返回 `nullopt`。`control_kick_user` 仍返回客户端 `seq`；要解析 `kicked_count` 等正文请用 `control_request`。`control_request` 发任意 CONTROL map（须含 `action`），可用的 action 包括：allowlist_list / add / update / delete，user_table_list / add / update / delete，以及 `list_users`、`kick_user`（与上两者等价但更通用）。示例见 `local/tests/test_admin.cpp`。

### 注意事项
- `sign_on的异常处理`：异常情况没有try/catch，在多种情况下会 throw std::runtime_error。需要新增一个异常检查机制，在sign_on前必须已open，否则没有有效连接。
- `底层asio的异常`：除 sign_on 显式抛的 runtime_error 外，底层 Asio read/write 也可能抛 boost::system::system_error。
- `sign_on客户端收LOGIN的reply的应答机制`：sign_on 当前实现没有用 seq 去对 LOGIN 应答，而是「登录阶段收到的第一个成功 LOGIN 回复即算数」；若协议上在 LOGIN 前混入了别的帧，会按上面的规则报错或丢弃逻辑而定。
- `积压情况`：积压的最终结局是变慢、被拒流、被断开或进程扛不住——不是无限排队保证送达。有三种情况，TCP窗口/反压（对端发太快、本机不读，接收窗口变小，对端 write 变慢或阻塞，表现为发送变慢）、中继进程（对每条连接有 queue_ + pending_bytes_，超过 high_water_bytes / hard_limit_bytes 可能 丢帧或断开）、内核缓冲（内核缓冲有上限，满后同样反压，极端情况下仍有 内存、句柄、延迟 问题）。
- `接收时的多线程设计`：作为 实时中继、字节直转、低持久化承诺 的组件，这种 长连 + 应用主动读 + 有限队列 是常见形态；要 强保证、可重放、对账，通常要在 业务层加 落库 / 消息队列 / 序号与补拉，不能默认全靠这条 TCP。实现多线程的话，需要使用此中间件的应用做主，SDK不提供。例如，可以：线程 A 阻塞 recv_deliver → 把 payload 丢队列 → 线程 B 写库或再 send。
- `普通用户调管理员方法的异常机制`：服务端会进行校验，不会执行具体action，客户端获得ok==false，统一报错为std::runtime_error。
