// ============================================================================
// 首次使用 asio-forwarder 的 C++ 示例（单进程内模拟「两个人」各一条 TCP）
//
// 你在本机需要已经：
//   1）MySQL 里建好库表（deliver/server/schema.sql）；
//   2）ip_allowlist 包含你的客户端源 IP（本机常为 127.0.0.1）；
//   3）users 里有本示例用的两个账号（见下方默认用户名），或改用你自己的；
//   4）./build/asio_forwarder 已启动，且 business 端口与下面 argv 一致。
//
// 构建（在仓库根已执行 ./scripts/build.sh 后）：
//   ./build/first_use_client 127.0.0.1 9000
//
// 测试数据：跑过 ./local/tests/run_e2e.sh 会在库 forwarder_e2e 里插入 e2e_alice / e2e_bob；
// 若用自己库，请手工 INSERT 两条用户或改代码里的名字与口令。
//
// 线协议与 HTTP 管理口：deliver/docs/protocol.md、deliver/docs/delivery.md
// ============================================================================

#include "fwd/forwarder_client.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace afc = fwd::asio_forwarder_client;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr
        << "用法: first_use_client <中继host> <业务TCP端口>\n"
        << "例:   first_use_client 127.0.0.1 9000\n"
        << "\n"
        << "说明: 本程序在同一进程里创建两条到中继的 TCP，分别登录 e2e_alice / e2e_bob，\n"
        << "      alice 向 bob 发一帧 DATA，bob 用 recv_deliver() 收到。\n";
    return 1;
  }

  const std::string host = argv[1];
  const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));

  // -------------------------------------------------------------------------
  // 可选：在发业务前探测「只读管理口」是否存活（配置里 admin.listen，常为 127.0.0.1:另一端口）
  // GET /api/health —— 与业务端口无关；这里仅作演示，端口需与 forwarder.json 一致。
  // const std::uint16_t admin_port = static_cast<std::uint16_t>(std::atoi(argv[3]));
  // if (!afc::admin_health_ok("127.0.0.1", admin_port)) { ... }
  // -------------------------------------------------------------------------

  // 每条 afc::Client 对应**一条**到中继的 TCP。典型业务是：一个进程一条连接；
  // 这里用两条 Client 在同一进程里演示「两个人」互相发。
  afc::Client alice;
  afc::Client bob;

  // open：只做 TCP connect，还不涉及登录。
  alice.open({host, port});
  bob.open({host, port});

  // sign_on：发送 LOGIN 并阻塞直到收到成功回复（失败会抛异常，详见实现）。
  // 参数：
  //   username / password：与 MySQL users 表一致（当前实现为明文口令）；
  //   RecvMode：对方给你发 DATA 时，若你名下有多条连接，如何选一条投递 —
  //            Broadcast = 每条连接都投；RoundRobin = 轮流选一条。
  //   peer_role：业务侧角色字符串，随 LOGIN 上报，供观测；默认 "user"。
  // peer_role 仅允许 "user" 或 "admin"（与 try_authenticate 一致），不可用任意字符串。
  alice.sign_on("e2e_alice", "secret1", afc::RecvMode::Broadcast, "user");
  bob.sign_on("e2e_bob", "secret2", afc::RecvMode::Broadcast, "user");

  // 登录成功后可用 local_username() 确认本连接绑定的登录名。
  std::cout << "alice 已登录为: " << alice.local_username() << '\n';
  std::cout << "bob   已登录为: " << bob.local_username() << '\n';

  // -------------------------------------------------------------------------
  // 发数据：send(目标登录名, 二进制载荷, SendOptions)
  //   wait_server_accept = true（默认）：会等到服务端 201「已受理」再返回（不保证对端 App 已处理）。
  //   false：只把帧推出去，不阻塞等对端的 201（高吞吐时可自行权衡）。
  const std::string payload = "hello from alice to bob";
  const std::uint32_t seq = alice.send("e2e_bob", payload, {/*wait_server_accept=*/true});
  std::cout << "alice 已发送 DATA，seq=" << seq << " len=" << payload.size() << '\n';

  // 收数据：recv_deliver() 阻塞直到收到一条「投递给本连接」的 DATA（封装为 200 deliver）。
  // 若你还需要收 201/Kick 等，可用底层 raw().recv() + visit（进阶，一般集成用高层 API 即可）。
  fwd::sdk::Deliver d = bob.recv_deliver();
  std::cout << "bob 收到投递: 来自=" << d.src_username << " 发往=" << d.dst_username
            << " payload=\"" << d.payload << "\"\n";
  afc::Client::log_deliver(d, std::cout);

  // 心跳：保持会话；长时间不发包时应周期性 heartbeat()，具体间隔见配置 session.heartbeat_timeout_ms。
  alice.heartbeat();
  bob.heartbeat();

  // -------------------------------------------------------------------------
  // 管理类（仅 is_admin=1 的账号可用）：列出在线用户、按「用户编号」踢下线。
  // user_id 来自 HTTP GET /api/users 或 LIST_USERS 的回复；本示例不演示管理员账号，
  // 种子用户 sc_admin / padm 可用于自行扩展试验。
  // alice.control_list_users();
  // alice.control_kick_user(target_user_id_64);
  // -------------------------------------------------------------------------

  // -------------------------------------------------------------------------
  // 其它 API（见 asio_forwarder_client.hpp）：
  //   try_login(host, port, user, pass, RecvMode, role) —— 仅探测能否登录，不保持连接。
  //   send_poly / send_typed —— 在二进制外包一层 msgpack 结构（type + data），见头文件。
  //   LocalForwarder —— 单元测试式启子进程 relay（Linux），一般集成用现成 asio_forwarder。
  //
  // 底层：client.raw() 得到 sdk::RelayClient，可 login/send_data/recv 更细粒度控制。
  // -------------------------------------------------------------------------

  std::cout << "演示结束。\n";
  return 0;
}
