// worker_cpp: receive tasks, process, return results to dispatcher by username.

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "fwd/relay_client.hpp"

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
              << " dst_conn=" << d->dst_conn_id << " payload=" << d->payload << "\n";

    // extremely small demo parsing: find "task_id":<n> and "a":<n> "b":<n>
    const std::string& s = d->payload;
    // 只处理 task；notice 仅展示不回包
    if (s.find("\"type\":\"task\"") == std::string::npos) {
      continue;
    }
    auto find_num = [&](const std::string& key) -> long long {
      auto p = s.find(key);
      if (p == std::string::npos) return 0;
      p += key.size();
      while (p < s.size() && (s[p] == ' ' || s[p] == ':')) ++p;
      long long v = 0;
      bool neg = false;
      if (p < s.size() && s[p] == '-') {
        neg = true;
        ++p;
      }
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        v = v * 10 + (s[p] - '0');
        ++p;
      }
      return neg ? -v : v;
    };

    const auto task_id = find_num("\"task_id\"");
    const auto a = find_num("\"a\"");
    const auto b = find_num("\"b\"");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto sum = a + b;

    const std::string res = std::string("{\"type\":\"result\",\"task_id\":") + std::to_string(task_id) + ",\"ok\":true,\"value\":" +
                            std::to_string(sum) + "}";
    c.send_unicast(dispatcher, res, 0);
    std::cout << "[worker] processed task_id=" << task_id << " sum=" << sum << "\n";
  }
  return 0;
}

