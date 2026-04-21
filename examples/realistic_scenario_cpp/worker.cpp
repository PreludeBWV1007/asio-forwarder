// worker_cpp: receive tasks, process, return results to dispatcher by username.

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "fwd/relay_client.hpp"
#include "messages.hpp"
#include "poly_messages.hpp"

static std::string arg_str(int argc, char** argv, const std::string& k, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == k) return argv[i + 1];
  }
  return def;
}
static bool has_flag(int argc, char** argv, const std::string& k) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == k) return true;
  }
  return false;
}

int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  const std::string host = arg_str(argc, argv, "--host", "127.0.0.1");
  const auto port = static_cast<std::uint16_t>(std::stoi(arg_str(argc, argv, "--port", "19000")));
  const std::string username = arg_str(argc, argv, "--username", "worker1");
  const std::string password = arg_str(argc, argv, "--password", username + "-pw");
  const std::string dispatcher = arg_str(argc, argv, "--dispatcher", "dispatcher");
  const bool reg = has_flag(argc, argv, "--register");

  fwd::sdk::RelayClient c;
  c.connect(host, port);
  c.login(username, password, "user", reg);

  std::cout << "[worker] up as " << username << ", waiting tasks\n";

  while (true) {
    auto ev = c.recv();
    if (!ev) break;
    if (auto* k = std::get_if<fwd::sdk::Kick>(&*ev)) {
      std::cout << "[worker] KICK: " << k->reason << "\n";
      return 2;
    }
    auto* d = std::get_if<fwd::sdk::Deliver>(&*ev);
    if (!d) continue;

    // 打印所有投递（便于看到 broadcast / round_robin 命中不同连接）
    std::cout << "[worker] deliver src=" << d->src_username << " dst=" << d->dst_username << " src_conn=" << d->src_conn_id
              << " dst_conn=" << d->dst_conn_id << " payload_bytes=" << d->payload.size()
              << "\n";

    std::unique_ptr<demo::PayloadBase> p;
    try {
      p = demo::decode_poly_payload(d->payload);
    } catch (...) {
      continue;
    }

    if (p->kind() == demo::PayloadKind::kNotice) {
      const auto* np = dynamic_cast<demo::NoticePayload*>(p.get());
      if (np) {
        const auto& n = np->notice;
        std::cout << "[worker] notice mode=" << n.mode << " msg=" << n.msg << " n=" << n.n << "\n";
      }
      continue;
    }
    if (p->kind() == demo::PayloadKind::kText) {
      const auto* tp = dynamic_cast<demo::TextPayload*>(p.get());
      if (tp) std::cout << "[worker] text msg=" << tp->text << "\n";
      continue;
    }
    if (p->kind() == demo::PayloadKind::kStockTick) {
      const auto* sp = dynamic_cast<demo::StockTickPayload*>(p.get());
      if (sp) {
        const auto& tk = sp->tick;
        std::cout << "[worker] tick symbol=" << tk.symbol << " last=" << tk.last << " vol=" << tk.volume
                  << " ts=" << tk.exchange_ts << "\n";
      }
      continue;
    }
    if (p->kind() != demo::PayloadKind::kTask) continue;

    const auto* taskp = dynamic_cast<demo::TaskPayload*>(p.get());
    if (!taskp) continue;
    const auto t = taskp->task;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto sum = t.a + t.b;

    demo::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    r.value = sum;
    demo::TaskResultPayload rp;
    rp.result = r;
    c.send_unicast(dispatcher, rp.pack(), 0);
    std::cout << "[worker] processed task_id=" << t.task_id << " sum=" << sum << "\n";
  }
  return 0;
}

