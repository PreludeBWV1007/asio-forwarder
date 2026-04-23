// admin_cpp: periodically list users (CONTROL), admin account only.

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
  const std::string username = arg_str(argc, argv, "--username", "admin");
  const std::string password = arg_str(argc, argv, "--password", "admin-pw");
  const bool reg = has_flag(argc, argv, "--register");

  fwd::sdk::RelayClient c;
  c.connect(host, port);
  c.login(username, password, "admin", reg);

  std::cout << "[admin] up. list_users every 3s\n";
  while (true) {
    c.control_list_users();
    // Read until we see one reply or kick
    while (true) {
      auto ev = c.recv();
      if (!ev) return 1;
      if (auto* k = std::get_if<fwd::sdk::Kick>(&*ev)) {
        std::cout << "[admin] KICK: " << k->reason << "\n";
        return 2;
      }
      if (auto* r = std::get_if<fwd::sdk::ServerReply>(&*ev)) {
        std::cout << "[admin] reply ok=" << (r->ok ? "true" : "false") << " op=" << r->op << " err=" << r->error
                  << " raw=" << r->raw << "\n";
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
}

