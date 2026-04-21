// dispatcher_cpp: send tasks to worker by username, receive results.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "fwd/relay_client.hpp"
#include "messages.hpp"

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
    c.send_unicast_typed(worker, "Task", t, 0);
    std::cout << "[dispatcher] sent task_id=" << task_id << "\n";
    ++task_id;

    // (B) broadcast demo: send a notice to all connections of the worker username
    if (demo_broadcast) {
      demo::Notice n;
      n.mode = "broadcast";
      n.msg = "hello-all-connections";
      n.n = task_id;
      c.send_broadcast_typed(worker, "Notice", n);
      std::cout << "[dispatcher] broadcast notice -> " << worker << "\n";
    }

    // (C) round robin demo: send a notice to all connections of the worker username with interval
    if (demo_round_robin) {
      demo::Notice n;
      n.mode = "round_robin";
      n.msg = "hello-round-robin";
      n.n = task_id;
      c.send_round_robin_typed(worker, "Notice", n, /*interval_ms*/ 50);
      std::cout << "[dispatcher] round_robin notice -> " << worker << " interval_ms=50\n";
    }

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
        if (d->typed && d->typed->type == "TaskResult") {
          auto r = d->typed->as<demo::TaskResult>();
          std::cout << "[dispatcher] result from " << d->src_username << " task_id=" << r.task_id << " ok=" << (r.ok ? "true" : "false")
                    << " value=" << r.value << "\n";
        } else {
          std::cout << "[dispatcher] deliver from " << d->src_username << " payload_bytes=" << d->payload.size()
                    << (d->typed ? " typed=" + d->typed->type : "") << "\n";
        }
      }
      // ignore replies
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

