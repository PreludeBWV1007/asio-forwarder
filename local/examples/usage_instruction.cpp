/* usage_instruction.cpp — C++ SDK 合一演示（可对照 deliver/docs/delivery.md、protocol.md）
 *
 * 前置条件
 * ---------
 * 1. MySQL 已按 deliver/server/schema.sql 建表，并已导入与本示例一致的账号（推荐直接导入
 *    local/tests/seed_e2e.sql：含 e2e_alice / e2e_bob、rr2_alice / rr2_bob、sc_admin 及 127.0.0.1 白名单）。
 * 2. asio_forwarder 已用「同一库」的配置启动；业务端口、可选管理 HTTP 端口与命令行一致。
 *
 * 构建与运行
 * ---------
 *   ./scripts/build.sh
 *   ./build/usage_instruction <host> <业务TCP端口> [管理HTTP端口]
 * 例：
 *   ./build/usage_instruction 127.0.0.1 19000 19003
 *
 * 输出说明
 * ---------
 * 每段以「=== 标题 ===」分隔；段内首行简述目的，PASS/FAIL 表示本段自检结论。
 * 管理员账号 sc_admin 登录时 peer_role 必须为 "admin"（与库里 is_admin=1 一致），否则服务端返回
 * 「登录所选权限与账号不一致」。
 *
 * SendOptions.wait_server_accept：发 DATA 后默认会读掉对应 201；若设为 false 且不自行 drain，
 * 下一次 recv_deliver 可能先读到 201 而抛错。
 */

#include "fwd/forwarder_client.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <msgpack.hpp>
#include <string>

namespace afc = fwd::asio_forwarder_client;

static void section(std::string_view title) {
  std::cout << "\n=== " << title << " ===\n";
}

static void note(std::string_view line) { std::cout << "说明: " << line << '\n'; }

static void print_hex(std::string_view bytes, std::size_t max_show = 48) {
  for (std::size_t i = 0; i < bytes.size() && i < max_show; ++i) {
    std::cout << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) << std::dec;
    if (i + 1 < bytes.size()) std::cout << ' ';
  }
  if (bytes.size() > max_show) std::cout << " ...";
  std::cout << '\n';
}

static void dump_control(std::string_view label, const msgpack::object& o) {
  std::cout << "  [" << label << "] ";
  std::cout << o << '\n';
}

/** 从 CONTROL 回复里 rows 数组按 username 找回 id（找不到返回 0） */
static std::uint64_t find_user_id_in_rows(const msgpack::object& root, const std::string& username) {
  if (root.type != msgpack::type::MAP) return 0;
  const auto& m = root.via.map;
  for (std::uint32_t i = 0; i < m.size; ++i) {
    if (m.ptr[i].key.type != msgpack::type::STR) continue;
    std::string k;
    m.ptr[i].key.convert(k);
    if (k != "rows" || m.ptr[i].val.type != msgpack::type::ARRAY) continue;
    const auto& a = m.ptr[i].val.via.array;
    for (std::uint32_t j = 0; j < a.size; ++j) {
      if (a.ptr[j].type != msgpack::type::MAP) continue;
      const auto& row = a.ptr[j].via.map;
      std::uint64_t id = 0;
      std::string un;
      for (std::uint32_t t = 0; t < row.size; ++t) {
        if (row.ptr[t].key.type != msgpack::type::STR) continue;
        std::string kk;
        row.ptr[t].key.convert(kk);
        if (kk == "id" && row.ptr[t].val.type == msgpack::type::POSITIVE_INTEGER) id = row.ptr[t].val.via.u64;
        if (kk == "username" && row.ptr[t].val.type == msgpack::type::STR) row.ptr[t].val.convert(un);
      }
      if (un == username) return id;
    }
  }
  return 0;
}

static std::uint64_t find_allowlist_id_for_ip(const msgpack::object& root, const std::string& ip) {
  if (root.type != msgpack::type::MAP) return 0;
  const auto& m = root.via.map;
  for (std::uint32_t i = 0; i < m.size; ++i) {
    if (m.ptr[i].key.type != msgpack::type::STR) continue;
    std::string k;
    m.ptr[i].key.convert(k);
    if (k != "rows" || m.ptr[i].val.type != msgpack::type::ARRAY) continue;
    const auto& a = m.ptr[i].val.via.array;
    for (std::uint32_t j = 0; j < a.size; ++j) {
      if (a.ptr[j].type != msgpack::type::MAP) continue;
      const auto& row = a.ptr[j].via.map;
      std::uint64_t id = 0;
      std::string sip;
      for (std::uint32_t t = 0; t < row.size; ++t) {
        if (row.ptr[t].key.type != msgpack::type::STR) continue;
        std::string kk;
        row.ptr[t].key.convert(kk);
        if (kk == "id" && row.ptr[t].val.type == msgpack::type::POSITIVE_INTEGER) id = row.ptr[t].val.via.u64;
        if (kk == "ip" && row.ptr[t].val.type == msgpack::type::STR) row.ptr[t].val.convert(sip);
      }
      if (sip == ip) return id;
    }
  }
  return 0;
}

/** Broadcast：同一用户多条连接时，每条都收到同一份 DATA（用 e2e_bob；首条连接已定为 Broadcast）。 */
static void demo_broadcast(const std::string& host, std::uint16_t biz, afc::Client& alice) {
  afc::Client b1;
  afc::Client b2;
  b1.open({host, biz});
  b2.open({host, biz});
  b1.sign_on("e2e_bob", "secret2", afc::RecvMode::Broadcast, "user");
  b2.sign_on("e2e_bob", "secret2", afc::RecvMode::Broadcast, "user");
  const std::string msg = "bcast-one-send-two-recv";
  note("alice -> e2e_bob；临时连接 b1、b2 与主线程里的 bob 同属一户，Broadcast 下每条连接各投一份。");
  (void)alice.send("e2e_bob", msg, {});
  fwd::sdk::Deliver a = b1.recv_deliver();
  fwd::sdk::Deliver b = b2.recv_deliver();
  const bool ok = (a.payload == msg && b.payload == msg);
  std::cout << "  结果 b1/b2 收到与发送相同负载: " << (ok ? "PASS" : "FAIL") << '\n';
}

/** RoundRobin：每条 DATA 只投到该用户的一条连接；须用「从未在本中继登录过」的账号首登为 RR，故示例用 rr2_bob。 */
static void demo_round_robin(const std::string& host, std::uint16_t biz) {
  afc::Client sender;
  sender.open({host, biz});
  sender.sign_on("rr2_alice", "p1", afc::RecvMode::Broadcast, "user");
  afc::Client rb1;
  afc::Client rb2;
  rb1.open({host, biz});
  rb2.open({host, biz});
  rb1.sign_on("rr2_bob", "p2", afc::RecvMode::RoundRobin, "user");
  rb2.sign_on("rr2_bob", "p2", afc::RecvMode::RoundRobin, "user");
  note("rr2_alice 连发两帧到 rr2_bob；期望两条连接各收一帧（顺序 rr-1、rr-2 在两连接上打散）。");
  (void)sender.send("rr2_bob", std::string("rr-1"), {});
  (void)sender.send("rr2_bob", std::string("rr-2"), {});
  fwd::sdk::Deliver f = rb1.recv_deliver();
  fwd::sdk::Deliver s = rb2.recv_deliver();
  const bool one_each =
      (f.payload == "rr-1" && s.payload == "rr-2") || (f.payload == "rr-2" && s.payload == "rr-1");
  std::cout << "  rb1 收: \"" << f.payload << "\"  rb2 收: \"" << s.payload << "\"  "
            << (one_each ? "PASS (各收一单)" : "FAIL (若首次登录该用户不是 RR 则会像 Broadcast)") << '\n';
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: usage_instruction <host> <业务TCP端口> [管理HTTP端口]\n";
    return 1;
  }
  const std::string host = argv[1];
  const std::uint16_t biz = static_cast<std::uint16_t>(std::atoi(argv[2]));
  const bool have_admin_http = (argc >= 4);
  const std::uint16_t admin_http = have_admin_http ? static_cast<std::uint16_t>(std::atoi(argv[3])) : std::uint16_t{0};

  section("0) 管理口探活（可选）");
  if (have_admin_http) {
    note("GET /api/health；第三参数为 admin.listen 端口（forwarder.json 默认 19003）。");
    std::cout << "  结果: " << (afc::admin_health_ok(host, admin_http) ? "PASS" : "FAIL") << '\n';
  } else {
    note("未传第三参数，跳过。");
  }

  section("1) try_login");
  note("短连接仅测 e2e_alice 能否登录，不占长连接。");
  std::cout << "  结果: " << (afc::try_login(host, biz, "e2e_alice", "secret1") ? "PASS" : "FAIL") << '\n';

  section("2) 长连接 + 二进制 / typed / poly DATA");
  note("alice、bob 均为 Broadcast；演示 send / send_typed / send_poly，bob 侧 recv_deliver。");

  afc::Client alice;
  afc::Client bob;
  alice.open({host, biz});
  bob.open({host, biz});
  alice.sign_on("e2e_alice", "secret1", afc::RecvMode::Broadcast, "user");
  bob.sign_on("e2e_bob", "secret2", afc::RecvMode::Broadcast, "user");

  (void)alice.send("e2e_bob", std::string("hello"), {});
  std::cout << "  bob 收明文: \"" << bob.recv_deliver().payload << "\" 期望 hello -> PASS\n";

  std::string bin;
  bin.push_back(static_cast<char>(0x08));
  bin.push_back(static_cast<char>(0xa8));
  bin.append("\0x", 2);
  (void)alice.send("e2e_bob", bin, {});
  std::cout << "  bob 收二进制(hex): ";
  print_hex(bob.recv_deliver().payload);

  (void)alice.send_typed("e2e_bob", "n", 42, {});
  {
    auto d = bob.recv_deliver();
    if (d.typed && d.typed->valid()) std::cout << "  bob send_typed 还原 int: " << d.typed->as<int>() << " 期望 42 -> PASS\n";
  }

  msgpack::sbuffer poly;
  {
    msgpack::packer<msgpack::sbuffer> pk(&poly);
    pk.pack_map(1);
    pk.pack("x");
    pk.pack(1);
  }
  {
    auto oh = msgpack::unpack(poly.data(), poly.size());
    (void)alice.send_poly("e2e_bob", 0, "t", oh.get(), {});
  }
  std::cout << "  bob send_poly 收包长度: " << bob.recv_deliver().payload.size() << " 字节\n";

  // 须 wait_server_accept=true（默认），否则残留 201 会导致下一次 recv_deliver 报错。
  (void)alice.send("e2e_bob", std::string("ff"), {});
  std::cout << "  bob 收: \"" << bob.recv_deliver().payload << "\"\n";

  (void)bob.send("e2e_alice", std::string("back"), {});
  std::cout << "  alice 收: \"" << alice.recv_deliver().payload << "\"\n";

  section("3) RecvMode：Broadcast");
  demo_broadcast(host, biz, alice);

  section("4) RecvMode：RoundRobin（独立用户 rr2_*）");
  demo_round_robin(host, biz);

  section("5) 心跳");
  note("刷新服务端 idle；发送后会等待 HEARTBEAT 的 201。");
  alice.heartbeat();
  bob.heartbeat();
  std::cout << "  结果: PASS\n";

  section("6) 管理员 CONTROL（sc_admin / peer_role=admin）");
  note("在线 list_users；ip_allowlist / users 表 CRUD。临时 IP/用户名仅供演示，会删除。");

  afc::Client adm;
  adm.open({host, biz});
  adm.sign_on("sc_admin", "padm", afc::RecvMode::Broadcast, "admin");

  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(1);
    pk.pack("action");
    pk.pack("list_users");
    dump_control("list_users 在线", adm.control_request(b).get());
  }

  const std::string demo_ip = "198.51.100.88";
  const std::string demo_user = "usage_demo_u";

  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(1);
    pk.pack("action");
    pk.pack("allowlist_list");
    dump_control("allowlist_list", adm.control_request(b).get());
  }
  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(2);
    pk.pack("action");
    pk.pack("allowlist_add");
    pk.pack("ip");
    pk.pack(demo_ip);
    dump_control("allowlist_add", adm.control_request(b).get());
  }
  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(1);
    pk.pack("action");
    pk.pack("allowlist_list");
    auto oh = adm.control_request(b);
    const std::uint64_t aid = find_allowlist_id_for_ip(oh.get(), demo_ip);
    if (aid != 0) {
      msgpack::sbuffer b2;
      msgpack::packer<msgpack::sbuffer> pk2(&b2);
      pk2.pack_map(3);
      pk2.pack("action");
      pk2.pack("allowlist_update");
      pk2.pack("id");
      pk2.pack(aid);
      pk2.pack("ip");
      pk2.pack(std::string("198.51.100.89"));
      dump_control("allowlist_update", adm.control_request(b2).get());
      msgpack::sbuffer b3;
      msgpack::packer<msgpack::sbuffer> pk3(&b3);
      pk3.pack_map(2);
      pk3.pack("action");
      pk3.pack("allowlist_delete");
      pk3.pack("id");
      pk3.pack(aid);
      dump_control("allowlist_delete", adm.control_request(b3).get());
    }
  }

  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(1);
    pk.pack("action");
    pk.pack("user_table_list");
    dump_control("user_table_list", adm.control_request(b).get());
  }
  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(4);
    pk.pack("action");
    pk.pack("user_table_add");
    pk.pack("username");
    pk.pack(demo_user);
    pk.pack("password");
    pk.pack(std::string("demo-pw"));
    pk.pack("is_admin");
    pk.pack(false);
    dump_control("user_table_add", adm.control_request(b).get());
  }
  {
    msgpack::sbuffer b;
    msgpack::packer<msgpack::sbuffer> pk(&b);
    pk.pack_map(1);
    pk.pack("action");
    pk.pack("user_table_list");
    auto oh = adm.control_request(b);
    const std::uint64_t uid = find_user_id_in_rows(oh.get(), demo_user);
    if (uid != 0) {
      msgpack::sbuffer b2;
      msgpack::packer<msgpack::sbuffer> pk2(&b2);
      pk2.pack_map(3);
      pk2.pack("action");
      pk2.pack("user_table_update");
      pk2.pack("id");
      pk2.pack(uid);
      pk2.pack("password");
      pk2.pack(std::string("new-pw"));
      dump_control("user_table_update", adm.control_request(b2).get());

      msgpack::sbuffer b3;
      msgpack::packer<msgpack::sbuffer> pk3(&b3);
      pk3.pack_map(2);
      pk3.pack("action");
      pk3.pack("user_table_delete");
      pk3.pack("id");
      pk3.pack(uid);
      dump_control("user_table_delete", adm.control_request(b3).get());
    }
  }

  section("结束");
  std::cout << "全部步骤跑完。若某段 FAIL，对照「说明」与 seed_e2e.sql / forwarder 日志。\n";
  return 0;
}
