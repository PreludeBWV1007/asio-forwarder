# 业务数据主链路详解

本文只讲 **转发器本体（C++）** 里「一条消息从上游进来，到所有下游收到」的路径，不涉及 Web sidecar、也不展开管理口 HTTP。读完应能回答：**数据在内存里长什么样、在哪些函数之间传递、为什么会排队/丢弃**。

相关代码主要在 `src/main.cpp`；协议字段在 `include/fwd/protocol.hpp`；帧格式摘要见 `docs/protocol.md`。

---

## 1. 先建立几个直觉

### 1.1 TCP 传的是「字节流」，不是「一条一条的消息」

TCP 保证**有序、可靠**地把字节送到对端，但**不会**自动帮你切分「第几条消息」。应用层要自己约定：**读多少字节算一帧**。

本项目的约定是：

- 先读 **固定 40 字节**（Header，v2）；
- Header 里有一个字段 **`body_len`**，再读 **`body_len` 字节**（Body）；
- 合起来就是一帧（Frame）。

这样无论数据是一次性到达还是分多次到达（半包），只要按「先 40、再 body_len」循环读，就能**重新拼出完整帧**。这就是文档里说的**粘包/半包**处理思路。

### 1.2 「上游一条、下游多条」在程序里是什么

- **上游**：程序认为**同一时刻只有一个** TCP 连接在「往转发器里灌数据」。新来的上游会**关掉**旧的（单槽位），避免多个生产者同时写导致语义复杂。
- **下游**：可以有**很多个** TCP 连接同时连着转发器；**同一帧**会尝试发给**每一个**还在线的下游（广播）。

### 1.3 核心语法库

| 概念 | 在本项目里大概干什么 |
|------|----------------------|
| `std::string` | 存一段连续字节（Header+Body 拼在一起就用它） |
| `std::shared_ptr<T>` | **多个对象共享同一块数据**，计数为 0 时自动释放。这里用来让「一帧」在多个下游队列里**只存一份字节**，避免复制 N 遍 |
| `std::vector` / `std::deque` | 容器；下游待发数据放在 `deque` 里，像「发送待办队列」 |
| `std::mutex` + `lock_guard` | 多线程同时访问同一份列表时**加锁**，避免错乱 |
| 异步 I/O（Boost.Asio） | 「发起读/写，先注册回调或协程，数据就绪再继续」，避免整个程序卡死在一条 `read` 上 |

 `upstream_readloop` 里的 `co_await` 「**在这里等待读操作完成，中间可以去处理别的连接**」，读完了再从下一行继续执行。实现上由 Asio 调度，不必手写复杂状态机。

---

## 2. 主链路总览（从连接到字节出去）

下面按**时间顺序**列出节点。端口以默认配置为例（`configs/dev/forwarder.json`）。

```text
[上游客户端] --TCP--> 本机 :19001
                          |
                    accept_upstream() 接受连接
                          |
                    upstream_（一条 socket）
                          |
                    upstream_readloop()  循环：
                      读 40B Header → 校验 → 读 body_len 的 Body
                          |
                    拼成一整段 bytes（shared_ptr<string>）
                          |
                    broadcast(bytes)
                          |
              +-----------+-----------+ ...
              |           |
        DownstreamSession  ...（每个下游一个）
              |
        每连接：strand 上排队 → async_write 发到 socket
              |
[下游客户端] <--TCP-- 本机 :19002
```

**一句话**：上游 socket 上**按协议切帧**；每一帧变成**一块共享的字节**；**对每个下游**把这块字节的**指针（shared_ptr）**放进自己的发送队列；各下游**各自慢慢往 TCP 里写**，互不完全拖死对方（慢下游有背压策略）。

---

## 3. 分步细讲：数据具体怎么「流动」

### 3.1 节点 A：监听并接受上游连接

- 程序启动后会对 **upstream 端口** 调用监听（`open_listen` + `accept_upstream`）。
- 每当有客户端连上，`async_accept` 的回调里会：
  - 若已有上游：关闭旧 socket，清空旧连接；
  - 把新连接放进 `upstream_`；
  - 调用 `start_upstream_readloop()`，启动**读帧循环**。

**数据形态**：此时还只是操作系统里的一个 **TCP socket**，还没有「一帧」的概念。

---

### 3.2 节点 B：`upstream_readloop`——从字节流里「抠」出一帧

这是主链路的核心。循环里每一轮大致做这些事（与 `src/main.cpp` 中 `upstream_readloop` 一致）：

1. **空闲超时**  
   若太久没有任何读成功，认为连接假死，退出循环并关闭连接。

2. **读 Header（恰好 40 字节）**  
   使用 `read_exact_with_timeout`：在限定时间内必须读满 40 字节，否则取消读并失败。  
   读到的 40 字节放在 `std::array<std::uint8_t, proto::Header::kHeaderLen>` 里。

3. **解析 Header**  
   `proto::Header::unpack_le` 把这 40 字节按**小端**解释成结构体字段（magic、version、body_len、src_user_id、dst_user_id 等）。

4. **校验**  
   - magic / version / header_len 必须合法；  
   - `body_len` 不能超过配置里的 `limits.max_body_len`（防止恶意或错误包撑爆内存）。

5. **读 Body（恰好 body_len 字节）**  
   若 `body_len == 0`，Body 为空；否则再 `read_exact_with_timeout` 读满 `body_len` 字节到 `std::string body`。

6. **统计入站**  
   `frames_in`、`bytes_in` 自增（原子变量，给 metrics 用）。

7. **拼成「线上格式」的一整块**  
   - `h.pack_le()` 再生成 40 字节的 header 字节（保证与线上一致）；  
   - 与 `body` 依次 append 到一个新的 `std::string`；  
   - 用 `std::make_shared<std::string>(...)` 包起来，得到 **`std::shared_ptr<std::string>`**。

8. **调用 `broadcast(out)`**  
   把这一帧交给下游扇出逻辑。

然后循环回到第 1 步，读下一帧。

**数据形态变化**：

```text
socket 上的原始字节流
    → 40B + body_len B（逻辑上的一帧）
    → 校验后的 Header 结构体 + body 字符串
    → 连续内存的一条 string（header+body）
    → shared_ptr<string>（供多下游共享）
```

---

### 3.3 节点 C：`broadcast`——把同一帧交给所有下游（不复制 N 份字节）

`broadcast` 做两件事：

1. **在锁 `down_mu_` 下**扫描当前维护的下游列表 `down_`，挑出 `is_open()` 的 `DownstreamSession`，拷贝到本地向量 `targets`；同时**压缩**列表，扔掉已断开的项（机会性清理）。

2. **释放锁后**，对 `targets` 里每一个 session 调用 `send_frame(bytes)`，**传入同一个 `shared_ptr<string>`**。

**要点**：多个下游拿到的**是同一块底层字节**（引用计数增加），不是每个下游各复制一整份 payload。这对大 Body 时省内存、省 CPU。

---

### 3.4 节点 D：`DownstreamSession::send_frame`——每个下游自己的「待发队列」

每个下游连接对应一个 `DownstreamSession` 对象，里面有自己的：

- `queue_`：`deque<shared_ptr<string>>`，待发帧队列；
- `pending_bytes_`：队列里所有帧的字节总和（用于背压）；
- `strand_`：保证**同一个连接**上的队列操作、写操作**串行**，避免多线程同时改队列。

`send_frame` 并不是立刻 `write`，而是把任务 **post 到 `strand_`** 上执行，在 strand 上：

1. 计算若把这帧入队，**队列总字节**是否超过配置：
   - 超过 **高水位** `high_water_bytes` 但未超过 **硬上限** `hard_limit_bytes`：  
     按 `on_high_water` 要么**丢这一帧**（`drop`），要么**断开该下游**（`disconnect`），并计 `drops`。
   - 超过 **硬上限**：**断开该下游**，防止内存无限增长。

2. 若通过检查：把 `shared_ptr` **push 到 `queue_`**，更新 `pending_bytes_`；若当前没有在写，则调用 `do_write()`。

**数据形态**：`shared_ptr<string>` 从 `broadcast` 传进来，**按引用**进入某个下游的 `queue_`（若未被丢弃）。

---

### 3.5 节点 E：`do_write`——真正往 TCP socket 里写

`do_write` 从队列头部取出一帧，对该下游的 `socket_` 调用 **`async_write`**（异步写整段 buffer）。

- 写**成功**：弹出队列头、`pending_bytes_` 减去该帧大小、`frames_out`/`bytes_out` 增加；若队列还有下一帧，继续 `do_write`。
- 写**失败**（例如对端关了连接）：标记停止、关闭 socket，该下游不再参与广播。

**数据形态**：`string` 里的字节通过内核发到 **TCP**，最终到达下游客户端。

---

## 4. 为什么「慢下游」不会卡死「上游读帧」？

- **上游**只在 `upstream_readloop` 里读和 `broadcast`；`broadcast` 对每个下游主要是 **post 到 strand + 入队**，一般很快返回。
- **真正可能耗时**的是每个下游自己的 **TCP 发送缓冲区** 和 **async_write 排队**；这些压力体现在**该下游的 `queue_` 变长**上。
- 当某下游队列太长，触发 **高水位/硬上限**，对该下游 **丢帧或断开**，**其它下游不受影响**。

这就是设计文档里说的：**背压按连接隔离**。

---

## 5. 与本文相关的源码锚点（便于对照阅读）

| 步骤 | 位置（文件） | 说明 |
|------|----------------|------|
| 协议头/打包 | `include/fwd/protocol.hpp` | `Header::unpack_le` / `pack_le` |
| 接受上游、单槽位 | `src/main.cpp` | `accept_upstream` |
| 读超时 | `src/main.cpp` | `read_exact_with_timeout` |
| 读帧循环 | `src/main.cpp` | `upstream_readloop` |
| 广播 | `src/main.cpp` | `broadcast` |
| 下游队列与写 | `src/main.cpp` | `DownstreamSession::send_frame`、`do_write` |
| 接受下游 | `src/main.cpp` | `accept_downstream` |

---

## 6. 常见问题

1. **一帧最少多少字节？**  
   只有 Header、Body 为空时，共 40 字节（`body_len == 0`）。

2. **同一条帧在内存里会复制几份？**  
   理想情况下**一份**字节，`shared_ptr` 被多个下游队列共享；若某下游丢弃该帧，只是不持有引用，不会为每个下游各拷贝一整份（除非实现变更）。

3. **上游断开后下游会怎样？**  
   已入下游队列的帧仍可能继续发送；**不再有新帧**。新上游连上后，重新从 `upstream_readloop` 开始读。

---

## 7. 延伸阅读

- 设计取舍与组件划分：`docs/design.md`  
- 路线图（规划中的 LOGIN/路由等）：`docs/architecture.md`  
- Header 字段表与字节序：`docs/protocol.md`  
- Body 使用 Protobuf、`WireMsgType` 与工具链：`docs/protobuf.md`  
- 业务侧可参考的组帧/读帧 POSIX 辅助：`include/fwd/frame_io.hpp`  
- 管理口、Web sidecar 与业务主链路的关系：`README.md`
