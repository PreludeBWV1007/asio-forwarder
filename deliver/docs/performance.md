# 性能与测试

## 基础功能测试

- `管理员功能`：`test_admin.cpp` 已覆盖管理员登录、60 秒空闲长连（内置约 20 秒一次心跳续命）、control_list_users、白名单与用户表 control_request、普通用户在线后再次列用户、control_kick_user 以及被踢后连接再发心跳应失败。

测试结果：

| 测试任务 | 测试内容 | 结果 |
|---------|----------|------|
| Test 1：管理员长连与空闲保活 | 使用管理员账号连接业务端口并以 `peer_role=admin` 完成 `sign_on`；随后空闲等待约 60 秒，验证在客户端内置周期心跳（约每 20 秒）协助下长连接仍保持、不断线。 | √ |
| Test 2：`control_list_users` | 管理员发起在线用户列表类 CONTROL，检查返回成功且正文含 `users`（或等价结构），能反映当前在线用户及连接信息。 | √（示例：仅 `sc_admin` 在线） |
| Test 3：`allowlist_list` | 查询 IP 白名单表全文，核对中继对允许接入的客户端地址配置可读、结构符合约定。 | √（示例含 `127.0.0.1`、`::1`） |
| Test 4：`user_table_list` | 读取 `users` 库表中账号列表（节选/行数），确认管理员可查当前库内注册用户概况；具体行数随库与历史测试残留而变。 | √（行数依库而定） |
| Test 5：白名单 CRUD | 对文档预留测试网段（如 RFC 5737 `192.0.2.x`）下的测试 IP 执行完整链路：`allowlist_add` → list 命中 → `allowlist_update` 改 IP → 再 list 校验 → `allowlist_delete` → 最终 list 中不再出现测试 IP。 | √ |
| Test 6a：`user_table` 与口令、`try_login` | 预先清理同名测试账号后：管理员 `user_table_add`、`user_table_list` 取 id、`user_table_update` 修改密码；再以短连接 `try_login` 断言新口令登录成功、旧口令被拒绝。 | √ |
| Test 6b：删库行后与「登录自动落库」 | 管理员 `user_table_delete` 删除刚维护的该行后，库里已无该用户名；使用相同用户名与**当前正确口令**再次 `try_login`，预期因登录路径自动 `INSERT` 而成功；`user_table_list` 校验新 `id` 与删前行 id 不同；最后清理测试用户名以免干扰后续步骤。 | √ |
| Test 7：普通用户 `e2e_alice` 上线 | 再起一条客户端会话，以种子数据中的普通用户账号登录业务口，使其处于在线状态，为后续踢人演示提供目标会话。 | √ |
| Test 8：再次列用户并解析 `user_id` | 管理员再次调用 `control_list_users`，从返回正文中解析出 `e2e_alice` 对应的 **`user_id`（数据库用户主键，非连接号）**，供 `kick_user` 精确指定目标用户。 | √（示例 `user_id`=1） |
| Test 9：`control_kick_user` | 管理员按解析得到的 `user_id` 发起踢人 CONTROL，服务端应终止该用户在当前中继上的全部业务连接并返回约定成功语义。 | √ |
| Test 10：被踢会话失效验证 | 在已被踢的用户连接上再次发送 `heartbeat`（或与读循环等价的写操作），预期失败（如对端已关连接时出现 `Broken pipe` 一类错误），确认踢人不仅应答成功且连接侧无法继续使用。 | √（示例：`Broken pipe`） |

- `基础用户功能`：`test_user` 在**双普通用户**场景下验证业务 TCP 上的 **DATA 透明转发**（含类券商侧**不透明小包**）、路由字段、显式 **heartbeat**、非管理员的 **CONTROL** 边界以及 **`try_login`**；**不包含**大单、吞吐与并发压测，亦与管理员踢人链路无关。

测试结果：

| 测试任务 | 测试内容 | 结果 |
|---------|----------|------|
| Test 1：双普通用户登录 | `e2e_alice`、`e2e_bob` 各占一条业务 TCP 会话，均以 `RecvMode::Broadcast`、`peer_role=user` 完成 `sign_on`。 | √ |
| Test 2：文本 DATA（Alice → Bob） | Alice 向 `e2e_bob` 发送短文本载荷，Bob `recv_deliver` 校验 `payload`、`src_username`、`dst_username` 与路由一致。 | √ |
| Test 3：文本 DATA 回复（Bob → Alice） | Bob 再向 Alice 回发另一段文本；Alice `recv_deliver` 内容与路由校验。 | √ |
| Test 4：显式 heartbeat | 双方各自调用一次 `heartbeat`，与内置周期心跳并存下仍应受理成功，连接保持可用。 | √ |
| Test 5：不透明二进制小包 | Alice 构造含 **NUL**、魔数前缀及 **0x00–0xFF** 字节的二进制串发往 Bob；Bob 收包后与发送端 **逐字节** 比对一致，验证中继不以「文本编码」曲解载荷。 | √（示例：**261** bytes） |
| Test 6：连续两帧保序 | Alice **无穿插**地向 Bob **连发两帧**不同前缀载荷；Bob 连续两次 `recv_deliver`，顺序须为「先第一帧、后第二帧」。 | √ |
| Test 7：零字节载荷 | Alice 发送 **0 字节** body（空 `std::string`），Bob 仍能收到一桩投递，`payload.size()==0`，路由字段正确。 | √ |
| Test 8：非管理员禁用 `control_list_users` | 仍以普通会话调用 `control_list_users(true)`，须失败并给出需管理员一类错误语义，而不得返回管理员列表正文。 | √（示例：需管理员权限） |
| Test 9：`try_login` 口令边界 | 短连 `try_login`：错误口令须为 false，种子账号正确口令须为 true。 | √ |
