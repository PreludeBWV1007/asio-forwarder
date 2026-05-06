# 使用方法

汇总项目各个模块的功能、原理、方法及使用方式。

## 客户端
引入的包是：`#include "fwd/asio_forwarder_client.hpp"`，对外的api在这里，命名空间是：fwd::asio_forwarder_client。

### 功能、原理和使用方法
- `Client类`：这是一个逻辑客户端，只是代表一个连接，即，一个 Client 实例 = 一条 TCP 连接 = 一次 sign_on 所作用的那条连接。注意，如果要同一个用户多个连接，需要建立新的Client。
- `Client类方法open`：实际实现是依赖内部的RelayClient::connect，用 Asio做解析主机名 + 建 TCP 连接（同步、阻塞直到连上或失败），新建一条从客户端到服务器的TCP连接，不包含账号登陆。该连接建立后的状态是【未登录】，首帧除LOGIN外，其他的都会被拒。
- `Client类方法sign_on`：在已建立的连接上登陆用户，携带用户名、密码、接收类型（RoundRobin/Broadcast）、用户类型（user/admin）。内部依赖RelayClient::login用msgpack打一个map，阻塞等待服务端的返回。服务端状态机解析并拆包，校验顺序是：报文完整性、IP白名单、用户账号密码、用户-连接表（连接队列/多连接更新、连接数上限），应答（失败则有提示信息）。客户端依赖RelayClient::recv接收，socket读满header，再按body_len读满body。
- `Client类方法heartbeat`：发心跳
- `Client类方法send/send_typed/send_ploy`：区别体现在payload形状上。send是任意字节，适合裸协议。send_typed适合结构化数据且不区分消息族/版本/子类型，用msgpack来包结构体，payload形状为{type, data}。send_typed适合结构化数据且不区分消息族/版本/子类型，结构为{kind, type, data}。
- `Client类方法recv_deliver`：底层是read凑齐到一帧，没有内置超时，所以外部看来，此函数是阻塞等待的。若想「等一阵没数据就做别的事」，要么 另一条线程 做别的事，要么 不用阻塞 API（自写 async_read+定时器、或 poll/select 带超时），要么在别的线程 close 连接 叫醒阻塞读。到达的数据会积存在本机TCP接收缓冲区。
- `Client类方法control_list_users/control_kick_user/control_request`：前两者是获得当前在线、按uid踢人（用户级踢人，被踢所有连接断开），第三个是通用管理员接口，除了list_users和kick_user之外，对于数据库的两张表的CRUD，可用的action为：list_users（返回users数组）、kick_user（携带target_user_id，返回kicked_count）、allowlist_list（返回ip白名单的数据库信息，id-ip对儿）、allowlist_add（携带ip）、allowlist_update（携带id、ip）、allowlist_delete（携带id）、user_table_list（返回users表的信息，id-username-is_admin对儿）、user_table_add（携带username、password、可选is_admin，默认false）、user_table_update（携带id，可选username/password/is_admin）、user_table_delete（携带id）。更新的方法见我们的例子。

### 注意事项
- `sign_on的异常处理`：异常情况没有try/catch，在多种情况下会 throw std::runtime_error。需要新增一个异常检查机制，在sign_on前必须已open，否则没有有效连接。
- `底层asio的异常`：除 sign_on 显式抛的 runtime_error 外，底层 Asio read/write 也可能抛 boost::system::system_error。
- `sign_on客户端收LOGIN的reply的应答机制`：sign_on 当前实现没有用 seq 去对 LOGIN 应答，而是「登录阶段收到的第一个成功 LOGIN 回复即算数」；若协议上在 LOGIN 前混入了别的帧，会按上面的规则报错或丢弃逻辑而定。
- `积压情况`：积压的最终结局是变慢、被拒流、被断开或进程扛不住——不是无限排队保证送达。有三种情况，TCP窗口/反压（对端发太快、本机不读，接收窗口变小，对端 write 变慢或阻塞，表现为发送变慢）、中继进程（对每条连接有 queue_ + pending_bytes_，超过 high_water_bytes / hard_limit_bytes 可能 丢帧或断开）、内核缓冲（内核缓冲有上限，满后同样反压，极端情况下仍有 内存、句柄、延迟 问题）。
- `接收时的多线程设计`：作为 实时中继、字节直转、低持久化承诺 的组件，这种 长连 + 应用主动读 + 有限队列 是常见形态；要 强保证、可重放、对账，通常要在 业务层加 落库 / 消息队列 / 序号与补拉，不能默认全靠这条 TCP。实现多线程的话，需要使用此中间件的应用做主，SDK不提供。例如，可以：线程 A 阻塞 recv_deliver → 把 payload 丢队列 → 线程 B 写库或再 send。
- `普通用户调管理员方法的异常机制`：服务端会进行校验，不会执行具体action，客户端获得ok==false，统一报错为std::runtime_error。
