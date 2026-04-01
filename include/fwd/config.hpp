#pragma once

// 本文件职责（配置结构）：
// - Config：转发器运行时配置（监听端口/线程数/超时/包长限制/背压阈值/metrics 周期等）
// - load_config_or_throw()：从 JSON 读取并校验配置（实现在 `src/main.cpp`）
// 典型配置示例：`configs/dev/forwarder.json`

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
    std::string on_high_water = "drop";                 // 高水位策略：drop | disconnect
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

}  // 命名空间 fwd

