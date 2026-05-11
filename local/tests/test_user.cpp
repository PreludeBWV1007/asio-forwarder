// test_user — 独立可执行：普通用户（非 admin）收发、权限边界。
// 业务视角：中继对上游下来的**不透明二进制**做按用户路由转发；本节仅做小包功能验证，大单与压测请另测。
// 依赖：中继 + seed_e2e.sql（e2e_alice/secret1、e2e_bob/secret2、127.0.0.1 白名单）。
// 用法: ./build/test_user <host> <业务TCP端口>
// 例:   ./build/test_user 127.0.0.1 19000

#include "fwd/forwarder_client.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace afc = fwd::asio_forwarder_client;

static void cout_section(const char* title) { std::cout << "\n========== " << title << " ==========\n"; }

/** 构造含 \\0 与 0x80–0xFF 字节序列，模拟字段式二进制切片（仍为小包）。 */
static std::string make_opaque_broker_like_payload() {
  std::string p;
  p.reserve(320);
  p.push_back('\0');
  const unsigned char tag[] = {0xCA, 0xFE, 0xBA, 0xBE};
  p.append(reinterpret_cast<const char*>(tag), sizeof(tag));
  for (int i = 0; i < 256; ++i) {
    p.push_back(static_cast<char>(i));
  }
  return p;
}

static bool deliver_matches(const fwd::sdk::Deliver& d, std::string_view expect_payload, std::string_view expect_src,
                            std::string_view expect_dst) {
  return d.payload == expect_payload && d.src_username == expect_src && d.dst_username == expect_dst;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: test_user <host> <业务TCP端口>\n";
    return 1;
  }
  const std::string host = argv[1];
  const std::uint16_t biz = static_cast<std::uint16_t>(std::atoi(argv[2]));

  try {
    cout_section("1 e2e_alice、e2e_bob 双端登录（Broadcast + user）");
    afc::Client alice;
    alice.open({host, biz});
    alice.sign_on("e2e_alice", "secret1", afc::RecvMode::Broadcast, "user");
    std::cout << "[OK] alice: " << alice.local_username() << '\n';

    afc::Client bob;
    bob.open({host, biz});
    bob.sign_on("e2e_bob", "secret2", afc::RecvMode::Broadcast, "user");
    std::cout << "[OK] bob: " << bob.local_username() << '\n';

    cout_section("2 alice → bob 发 DATA（文本），bob recv_deliver");
    const std::string payload_a2b = "hello-from-alice";
    (void)alice.send("e2e_bob", payload_a2b, {});
    auto d1 = bob.recv_deliver();
    if (!deliver_matches(d1, payload_a2b, "e2e_alice", "e2e_bob")) {
      std::cout << "[FAIL] 投递内容与路由不符合预期\n";
      afc::Client::log_deliver(d1, std::cout);
      return 2;
    }
    std::cout << "[OK] bob 收到: bytes=" << d1.payload.size() << '\n';
    afc::Client::log_deliver(d1, std::cout);

    cout_section("3 bob → alice 回复，alice recv_deliver");
    const std::string payload_b2a = "reply-from-bob";
    (void)bob.send("e2e_alice", payload_b2a, {});
    auto d2 = alice.recv_deliver();
    if (!deliver_matches(d2, payload_b2a, "e2e_bob", "e2e_alice")) {
      std::cout << "[FAIL] 回投递不符合预期\n";
      afc::Client::log_deliver(d2, std::cout);
      return 3;
    }
    std::cout << "[OK] alice 收到回复\n";
    afc::Client::log_deliver(d2, std::cout);

    cout_section("4 显式 heartbeat（可与内置周期心跳并存）");
    alice.heartbeat();
    bob.heartbeat();
    std::cout << "[OK] 双方 heartbeat OK\n";

    cout_section("5 alice → bob 不透明二进制（含 NUL 与高字节），逐字节一致");
    const std::string bin_payload = make_opaque_broker_like_payload();
    (void)alice.send("e2e_bob", bin_payload, {});
    auto dbin = bob.recv_deliver();
    if (!deliver_matches(dbin, bin_payload, "e2e_alice", "e2e_bob")) {
      std::cout << "[FAIL] 二进制载荷往返不一致或路由错误\n";
      afc::Client::log_deliver(dbin, std::cout);
      return 4;
    }
    std::cout << "[OK] 二进制载荷 bytes=" << dbin.payload.size() << "（与发送端逐字节相同）\n";

    cout_section("6 alice → bob 连发两帧，bob 按发送顺序收齐（保序）");
    const std::string first = std::string("\x01", 1) + "frame-a";
    const std::string second = std::string("\x02", 1) + "frame-b";
    (void)alice.send("e2e_bob", first, {});
    (void)alice.send("e2e_bob", second, {});
    auto o1 = bob.recv_deliver();
    auto o2 = bob.recv_deliver();
    if (!deliver_matches(o1, first, "e2e_alice", "e2e_bob") || !deliver_matches(o2, second, "e2e_alice", "e2e_bob")) {
      std::cout << "[FAIL] 双帧顺序或内容与路由不符合预期\n";
      afc::Client::log_deliver(o1, std::cout);
      afc::Client::log_deliver(o2, std::cout);
      return 5;
    }
    std::cout << "[OK] 先收 frame-a 再收 frame-b\n";

    cout_section("7 alice → bob 空载荷（0 字节 body）");
    const std::string empty_pl;
    (void)alice.send("e2e_bob", empty_pl, {});
    auto de = bob.recv_deliver();
    if (!deliver_matches(de, empty_pl, "e2e_alice", "e2e_bob")) {
      std::cout << "[FAIL] 空载荷投递不符合预期\n";
      afc::Client::log_deliver(de, std::cout);
      return 6;
    }
    std::cout << "[OK] 空载荷投递 bytes=0\n";

    cout_section("8 普通用户调用 control_list_users 应失败");
    try {
      (void)alice.control_list_users(true);
      std::cout << "[FAIL] 非管理员应无法 list_users\n";
      return 7;
    } catch (const std::exception& e) {
      std::cout << "[OK] 预期异常: " << e.what() << '\n';
    }

    cout_section("9 try_login 错误口令应 false，正确口令应 true");
    if (afc::try_login(host, biz, "e2e_alice", "wrong-password", afc::RecvMode::Broadcast, "user")) {
      std::cout << "[FAIL] 错误口令不应登录成功\n";
      return 8;
    }
    if (!afc::try_login(host, biz, "e2e_alice", "secret1", afc::RecvMode::Broadcast, "user")) {
      std::cout << "[FAIL] 正确口令应探测成功\n";
      return 9;
    }
    std::cout << "[OK] try_login 边界符合预期\n";

    std::cout << "\n========== 全部步骤完成 ==========\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[FAIL] " << e.what() << '\n';
    return 10;
  }
}
