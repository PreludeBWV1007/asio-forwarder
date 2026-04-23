// =============================================================================
// tests/cpp_smoke.cpp —— C++ SDK 黑盒回归（可执行名 build/forwarder_cpp_smoke，非最终用户交付物）
//
// 你要加自己的黑盒测什么？
//   · 最省事：改/扩本文件里的 smoke_test()，在对应步骤旁加断言语句或新代码块。
//   · 想干净拆开：在 tests/ 下新建用例，CMake 里 add_executable，链 asio_forwarder_sdk。
//   · API 在 include/fwd/asio_forwarder_client.hpp ；多进程大示例见 examples/realistic_scenario_cpp/ 。
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <variant>
#include <vector>

#include "fwd/asio_forwarder_client.hpp"
#include "messages.hpp"

namespace afc = fwd::asio_forwarder_client;

namespace {

struct Run {
  std::string host{"127.0.0.1"};
  std::uint16_t client_port{0};
  std::uint16_t admin_port{0};
  std::string tag;
  std::uint32_t stress_n{2000};
  int stability_rounds{100};
  bool no_probes{false};
};

void print_usage() {
  std::cerr << "forwarder_cpp_smoke\n"
               "  [--connect HOST PORT [ADMIN]] [--stress-n N] [--stability-rounds R] [--no-probes]\n";
}

struct Cmd {
  bool help{false};
  bool use_remote{false};
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint16_t admin{0};
  std::uint32_t stress_n{2000};
  int stability{100};
  bool no_probes{false};
};

Cmd parse(int argc, char** argv) {
  Cmd c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      c.help = true;
      return c;
    }
    if (a == "--connect") {
      c.use_remote = true;
      if (i + 2 >= argc) throw std::runtime_error("--connect 需 HOST 与 PORT");
      c.host = argv[++i];
      c.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
      if (i + 1 < argc && argv[i + 1][0] != '-') c.admin = static_cast<std::uint16_t>(std::stoi(argv[++i]));
      continue;
    }
    if (a == "--stress-n" && i + 1 < argc) {
      c.stress_n = static_cast<std::uint32_t>(std::stoul(argv[++i]));
      continue;
    }
    if (a == "--stability-rounds" && i + 1 < argc) {
      c.stability = std::stoi(argv[++i]);
      continue;
    }
    if (a == "--no-probes") {
      c.no_probes = true;
      continue;
    }
    throw std::runtime_error(std::string("未知: ") + a);
  }
  return c;
}

// 金融/高可用场景下常用「边界/异常路径/容量」探针。失败时 throw 阻断；观察项会打印到 stdout 供排障/评审。
// 与实现细节相关的已知语义（不自动判失败）也会打 [probe] 观察 行，便于和协议/运维约定对齐。
void reliability_probes(const Run& r, afc::Client& desk, afc::Client& md) {
  const afc::ConnectionConfig cc{r.host, r.client_port};
  const std::string pw = "demo-pw";
  // 新的一段探针前刷新两端心跳，并避免 H3 仅「收单播、偶发发心跳」时接收端在服务端上长时间无入站业务帧
  md.heartbeat();
  desk.heartbeat();

  // H1 空 payload 往返
  (void)desk.send(afc::SendMode::Unicast, md.local_username(), std::string{});
  if (md.recv_deliver().payload != "") throw std::runtime_error("probe: empty payload");

  // H2 较大 payload。服务端在「整段 DATA 读入并 handle」后才 touch 发端；极大帧 + 极慢机可能拉长该窗口，故用 8KB 作上限回归样本
  {
    const std::size_t k = 8u * 1024u;
    std::string big(k, 0);
    for (std::size_t i = 0; i < k; ++i) big[i] = static_cast<char>(static_cast<unsigned char>(i) & 0xFFu);
    (void)desk.send(afc::SendMode::Unicast, md.local_username(), big);
    if (md.recv_deliver().payload != big) throw std::runtime_error("probe: large_8k");
  }

  // H3 小报文 micro-burst。接收端在 recv 中不占「客户端→服务器」的入站；长循环须穿插 heartbeat（宜在收完一帧后调，见 Client::expect_heartbeat_ok）。
  {
    const int burst = static_cast<int>(std::min<std::uint32_t>(std::min(r.stress_n, 200u), 100u));
    for (int i = 0; i < burst; ++i) {
      const char c = static_cast<char>('0' + (i % 10));
      (void)desk.send(afc::SendMode::Unicast, md.local_username(), std::string(1, c));
      if (md.recv_deliver().payload.size() != 1u) throw std::runtime_error("probe: micro_burst");
      if ((i % 2) == 0) {
        md.heartbeat();
        desk.heartbeat();
      }
    }
  }

  md.heartbeat();
  desk.heartbeat();

  // H4 不存在目标用户：当前实现可能仍 201 受理、但不投递；业务侧应自行判断/监控
  (void)desk.send(afc::SendMode::Unicast, "___nouser_" + r.tag, "orphan", {});
  std::cout << "[probe] 观察: 对不存在 dst_username 发单播时，对端无投递，但本实现仍对发送方 201 受理；"
               "发方应用勿仅凭 ACK 当「对端已收」。\n";

  // H5 错误 dst_conn_id：先对齐队列，再发一条不可达 unicast，再发正常 ping
  (void)desk.send(afc::SendMode::Unicast, md.local_username(), "pre-bad-conn", {});
  if (md.recv_deliver().payload != "pre-bad-conn") throw std::runtime_error("probe: pre-bad-conn");
  {
    afc::SendOptions bad{};
    bad.dst_conn_id = 0xFFFF'FFFF'0000'0001ULL;  // 不存在的连接 id
    (void)desk.send(afc::SendMode::Unicast, md.local_username(), "lost", bad);
  }
  (void)desk.send(afc::SendMode::Unicast, md.local_username(), "post-bad-conn", {});
  if (md.recv_deliver().payload != "post-bad-conn") throw std::runtime_error("probe: bad_conn_id 后应仍能收到后序单播");
  std::cout << "[probe] 观察: dst_conn_id 无匹配时无 DELIVER，但 201 仍可能成功；对端不消耗一条 recv。\n";

  md.heartbeat();
  desk.heartbeat();

  // H6 非 admin 发 CONTROL（期望 201 ok:false 带「需要管理员…」；若已 KICK/断连应失败整个探针，勿与权限判混）
  {
    bool threw = false;
    try {
      (void)desk.control_list_users(true);
    } catch (const std::exception& e) {
      threw = true;
      const std::string w = e.what();
      if (w.find("KICK") != std::string::npos || w.find("EOF") != std::string::npos) {
        throw;
      }
      if (w.find("需要管理员") == std::string::npos && w.find("CONTROL 未受理") == std::string::npos) {
        std::cout << "[probe] 注意: 非 admin CONTROL 未命中预期子串: " << w << "\n";
      }
    }
    if (!threw) throw std::runtime_error("probe: 非 admin 的 CONTROL 应失败");
  }

  // H7 长窗口仅心跳 + 再业务（模拟业务间隙；两路都发 HEARTBEAT，避免只一侧保活时另一侧因无帧被踢）
  {
    for (int i = 0; i < 4; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      md.heartbeat();
      desk.heartbeat();
    }
    (void)desk.send(afc::SendMode::Unicast, md.local_username(), "after-idle", {});
    if (md.recv_deliver().payload != "after-idle") throw std::runtime_error("probe: long_idle+heartbeat");
  }

  // H8 第 9 路同用户连接 → 最旧一路 KICK（策略：fifo 丢最老）
  {
    const std::string slim_user = "slim_" + r.tag;
    std::vector<std::unique_ptr<afc::Client>> v;
    v.push_back(std::make_unique<afc::Client>());
    v[0]->open(cc);
    v[0]->sign_on(slim_user, pw, true);

    std::atomic<bool> got_kick{false};
    std::thread watch([&] {
      for (;;) {
        auto ev = v[0]->raw().recv();
        if (!ev) return;
        if (std::get_if<fwd::sdk::Kick>(&*ev) != nullptr) {
          got_kick = true;
          return;
        }
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i = 0; i < 7; ++i) {
      v.push_back(std::make_unique<afc::Client>());
      v.back()->open(cc);
      v.back()->sign_on(slim_user, pw, false);
    }
    v.push_back(std::make_unique<afc::Client>());
    v.back()->open(cc);
    v.back()->sign_on(slim_user, pw, false);
    watch.join();
    if (!got_kick.load()) throw std::runtime_error("probe: max_conn 未对最旧连接发 KICK");
  }
  std::cout << "[probe] 可靠性探针: 全部强断言项通过；请一并阅读上文「观察」行（若有）。\n";
}

// 本仓库自带的「一条龙」自测：单播 → typed →（可选 H 探针）→ 广播 → 稳态 → 吞吐 → 错口令/重复注册。
// 你写自己的用例时：复制本函数一段，或在本函数尾部追加步骤；失败用 throw 即可让 main 打 FAIL。
void smoke_test(const Run& r) {
  const afc::ConnectionConfig cc{r.host, r.client_port};
  const std::string pw = "demo-pw";

  // ---------- A) 建两个已登录端：desk 当发送方、md 当接收方 ----------
  afc::Client desk;
  desk.open(cc);
  desk.sign_on("desk_" + r.tag, pw, true);

  afc::Client md;
  md.open(cc);
  md.sign_on("md_" + r.tag, pw, true);

  // ---------- B) 最简单播：发原始字节，收端打一行摘要 ----------
  (void)desk.send(afc::SendMode::Unicast, md.local_username(), "ping-" + r.tag);
  afc::Client::log_deliver(md.recv_deliver());

  // ---------- C) 单播 + SDK typed 信封：结构体发过去，对端可解析 typed ----------
  demo::StockTick t{};
  t.symbol = "600519.SH";
  t.exchange_ts = 1;
  t.last = 1700.0;
  t.open = t.high = t.low = t.prev_close = 1690.0;
  t.volume = 1;
  t.turnover = 1.0;
  t.is_trading = true;
  (void)desk.send_typed<demo::StockTick>(afc::SendMode::Unicast, md.local_username(), "StockTick", t);
  const auto d2 = md.recv_deliver();
  afc::Client::log_deliver(d2);
  if (d2.typed) (void)d2.typed->as<demo::StockTick>();

  // ---------- H) 可靠性/边界探针：放在长耗时段 E/F 与 G 之前 —— 否则「仅收单播」一端可能长时间对服务端无入站帧，触发心跳踢线 ----------
  if (!r.no_probes) {
    reliability_probes(r, desk, md);
  }

  // ---------- D) 同用户两连接 + broadcast：两线程各 recv，再发一条广播 ----------
  // md2 仅本段需要；若留到 E/F/H 长耗时段，该连接会因无流量触发心跳超时（与 E 段注释同一类问题）。
  {
    afc::Client md2;
    md2.open(cc);
    md2.sign_on(md.local_username(), pw, false);
    const std::string b = "bcast-" + r.tag;
    std::atomic<int> ok{0};
    std::thread u([&] {
      if (md.recv_deliver().payload == b) ok.fetch_add(1);
    });
    std::thread v([&] {
      if (md2.recv_deliver().payload == b) ok.fetch_add(1);
    });
    (void)desk.send(afc::SendMode::Broadcast, md.local_username(), b);
    u.join();
    v.join();
    if (ok.load() != 2) throw std::runtime_error("broadcast 未双收");
  }

  // ---------- E) 稳定性：多轮「心跳 + 小单播」，防长时间只 recv 被踢 ----------
  for (int i = 0; i < r.stability_rounds; ++i) {
    md.heartbeat();
    const std::string m = "st-" + std::to_string(i);
    (void)desk.send(afc::SendMode::Unicast, md.local_username(), m);
    if (md.recv_deliver().payload != m) throw std::runtime_error("stability");
  }

  // ---------- F) 吞吐：大量单播往返；接收端每 10 次补心跳 ----------
  std::string buf(256, 'x');
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < r.stress_n; ++i) {
    if (i % 10u == 0u) md.heartbeat();
    buf[0] = static_cast<char>('A' + static_cast<int>(i % 26));
    (void)desk.send(afc::SendMode::Unicast, md.local_username(), buf);
    if (md.recv_deliver().payload.size() != buf.size()) throw std::runtime_error("throughput");
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "[throughput] n=" << r.stress_n << " ms=" << ms << "\n";
  // 吞吐段末尾 md 在 i=..490 时心跳后可能连续多轮仅收不发；补一轮双端 HEARTBEAT 再进 G/H，防会话计时漂移
  md.heartbeat();
  desk.heartbeat();

  // ---------- G) 边界：错口令、重复注册（独立临时建连，不动 desk/md）----------
  if (afc::try_login(r.host, r.client_port, desk.local_username(), "badpw", "user")) {
    throw std::runtime_error("错口令应失败");
  }
  const std::string dupu = "dup_" + r.tag;
  if (!afc::try_register(r.host, r.client_port, dupu, pw)) throw std::runtime_error("首注册应成功");
  if (afc::try_register(r.host, r.client_port, dupu, pw)) throw std::runtime_error("重注册应失败");
}

}  // namespace

int main(int argc, char** argv) {
  // ① 读命令行：无参 = 本进程 fork 起临时 asio_forwarder；--connect = 你已有进程
  Cmd cmd;
  try {
    cmd = parse(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    print_usage();
    return 2;
  }
  if (cmd.help) {
    print_usage();
    return 0;
  }

  afc::LocalForwarder local;
  Run r;
  r.tag = std::to_string(static_cast<int>(::getpid()));
  r.stress_n = cmd.stress_n;
  r.stability_rounds = cmd.stability;
  r.no_probes = cmd.no_probes;

  try {
    if (cmd.use_remote) {
      if (cmd.port == 0) throw std::runtime_error("port");
      r.host = cmd.host;
      r.client_port = cmd.port;
      r.admin_port = cmd.admin;
    } else {
      local.start();
      r.host = std::string(local.loopback_host());
      r.client_port = local.client_tcp_port();
      r.admin_port = local.admin_http_port();
    }

    if (r.admin_port != 0 && !afc::admin_health_ok(r.host, r.admin_port)) throw std::runtime_error("/api/health");
    if (r.admin_port != 0) std::cout << "[admin] health OK\n";

    // ② 黑盒用例都写进 smoke_test；你也可在此再调自己的 void my_test(const Run&)
    smoke_test(r);

    local.stop();
    std::cout << "smoke: OK\n";
    return 0;
  } catch (const std::exception& e) {
    local.stop();
    std::cerr << "smoke: FAIL: " << e.what() << "\n";
    return 1;
  }
}
