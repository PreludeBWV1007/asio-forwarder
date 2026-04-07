#pragma once

// 配置：单端口对等客户端接入 + admin HTTP + 会话/转发策略
// load_config_or_throw() 在 `src/main.cpp`

#include <cstdint>
#include <string>

#include "fwd/session_policy.hpp"

namespace fwd {

struct Config {
  struct Listen {
    std::string host = "0.0.0.0";
    std::uint16_t port = 0;
    int backlog = 1024;
  };

  struct Admin {
    Listen listen{.host = "127.0.0.1", .port = 19003, .backlog = 128};
    int events_max = 200;
  };

  struct Timeouts {
    int read_ms = 15000;
    int idle_ms = 120000;
  };

  struct Limits {
    std::uint32_t max_body_len = 64 * 1024 * 1024;
  };

  struct FlowControl {
    std::uint32_t high_water_bytes = 64 * 1024 * 1024;
    std::uint32_t hard_limit_bytes = 256 * 1024 * 1024;
    std::string on_high_water = "drop";
  };

  struct Metrics {
    int interval_ms = 5000;
  };

  Listen client_listen{.port = 9000};
  Admin admin{};
  Timeouts timeouts{};
  Limits limits{};
  FlowControl flow{};
  Metrics metrics{};
  SessionPolicy session{};

  int io_threads = 2;
  int biz_threads = 2;
};

Config load_config_or_throw(const std::string& path);

}  // namespace fwd
