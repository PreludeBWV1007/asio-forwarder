#pragma once

#include <cstdint>
#include <string>

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
    int idle_ms = 60000;
  };

  struct Limits {
    std::uint32_t max_body_len = 4 * 1024 * 1024;  // 4MB
  };

  struct FlowControl {
    std::uint32_t high_water_bytes = 4 * 1024 * 1024;   // 4MB
    std::uint32_t hard_limit_bytes = 16 * 1024 * 1024;  // 16MB
    std::string on_high_water = "drop";                 // drop | disconnect
  };

  struct Metrics {
    int interval_ms = 5000;
  };

  Listen upstream_listen{.port = 9001};
  Listen downstream_listen{.port = 9002};
  Admin admin{};
  Timeouts timeouts{};
  Limits limits{};
  FlowControl flow{};
  Metrics metrics{};

  int io_threads = 2;
  int biz_threads = 2;
};

Config load_config_or_throw(const std::string& path);

}  // namespace fwd

