// dispatcher_cpp: send tasks to worker by username, receive results.

#include <chrono>
#include <cstdlib>
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
  const std::string username = arg_str(argc, argv, "--username", "dispatcher");
  const std::string password = arg_str(argc, argv, "--password", "dispatcher-pw");
  const std::string worker = arg_str(argc, argv, "--worker", "worker1");
  const bool reg = has_flag(argc, argv, "--register");
  const bool demo_broadcast = has_flag(argc, argv, "--demo-broadcast");
  const bool demo_round_robin = has_flag(argc, argv, "--demo-round-robin");

  fwd::sdk::RelayClient c;
  c.connect(host, port);
  c.login(username, password, "user", reg);

  std::uint64_t task_id = 1;
  std::cout << "[dispatcher] up. send tasks to " << worker << "\n";
  if (demo_broadcast) std::cout << "[dispatcher] demo: broadcast enabled (to dst_username=" << worker << ")\n";
  if (demo_round_robin) std::cout << "[dispatcher] demo: round_robin enabled (to dst_username=" << worker << ")\n";

  while (true) {
    // (A) unicast task -> worker (worker will respond)
    demo::Task t;
    t.task_id = task_id;
    t.a = static_cast<std::int64_t>(task_id);
    t.b = static_cast<std::int64_t>(task_id + 1);
    demo::TaskPayload tp;
    tp.task = t;
    c.send_unicast(worker, tp.pack(), 0);
    std::cout << "[dispatcher] sent task_id=" << task_id << "\n";
    ++task_id;

    // (B) broadcast demo: send a notice to all connections of the worker username
    if (demo_broadcast) {
      demo::Notice n;
      n.mode = "broadcast";
      n.msg = "hello-all-connections";
      n.n = task_id;
      demo::NoticePayload np;
      np.notice = n;
      c.send_broadcast(worker, np.pack());
      std::cout << "[dispatcher] broadcast notice -> " << worker << "\n";
    }

    // (C) round robin demo: send a notice to all connections of the worker username with interval
    if (demo_round_robin) {
      demo::Notice n;
      n.mode = "round_robin";
      n.msg = "hello-round-robin";
      n.n = task_id;
      demo::NoticePayload np;
      np.notice = n;
      c.send_round_robin(worker, np.pack(), /*interval_ms*/ 50);
      std::cout << "[dispatcher] round_robin notice -> " << worker << " interval_ms=50\n";
    }

    // (D) also send a text message (poly kind=Text)
    demo::TextPayload txt;
    txt.text = "dispatcher says hi";
    c.send_unicast(worker, txt.pack(), 0);

    // (E) send a stock tick (poly kind=StockTick)
    demo::StockTickPayload st;
    st.tick.symbol = "600519.SH";
    st.tick.exchange_ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    st.tick.last = 1688.5;
    st.tick.open = 1670.0;
    st.tick.high = 1699.0;
    st.tick.low = 1666.0;
    st.tick.prev_close = 1668.0;
    st.tick.volume = 123456;
    st.tick.turnover = 2.34e8;
    st.tick.is_trading = true;
    c.send_unicast(worker, st.pack(), 0);

    // receive results briefly
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      auto ev = c.recv();
      if (!ev) break;
      if (auto* k = std::get_if<fwd::sdk::Kick>(&*ev)) {
        std::cout << "[dispatcher] KICK: " << k->reason << "\n";
        return 2;
      }
      if (auto* d = std::get_if<fwd::sdk::Deliver>(&*ev)) {
        try {
          auto p = demo::decode_poly_payload(d->payload);
          if (p->kind() == demo::PayloadKind::kTaskResult) {
            const auto* pr = dynamic_cast<demo::TaskResultPayload*>(p.get());
            if (pr) {
              const auto& r = pr->result;
              std::cout << "[dispatcher] result from " << d->src_username << " task_id=" << r.task_id
                        << " ok=" << (r.ok ? "true" : "false") << " value=" << r.value << "\n";
            }
          } else if (p->kind() == demo::PayloadKind::kText) {
            const auto* pt = dynamic_cast<demo::TextPayload*>(p.get());
            std::cout << "[dispatcher] text from " << d->src_username << " msg=" << (pt ? pt->text : "") << "\n";
          } else if (p->kind() == demo::PayloadKind::kStockTick) {
            const auto* ps = dynamic_cast<demo::StockTickPayload*>(p.get());
            if (ps) {
              const auto& tk = ps->tick;
              std::cout << "[dispatcher] tick from " << d->src_username << " symbol=" << tk.symbol << " last=" << tk.last
                        << " vol=" << tk.volume << " ts=" << tk.exchange_ts << "\n";
            }
          } else {
            std::cout << "[dispatcher] deliver from " << d->src_username << " kind=" << p->type_name()
                      << " payload_bytes=" << d->payload.size() << "\n";
          }
        } catch (...) {
          std::cout << "[dispatcher] deliver from " << d->src_username << " payload_bytes=" << d->payload.size()
                    << " (not poly)" << "\n";
        }
      }
      // ignore replies
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

