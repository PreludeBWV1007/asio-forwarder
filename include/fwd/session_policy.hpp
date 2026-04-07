#pragma once

#include <cstdint>

namespace fwd {

// 会话与转发策略（对等客户端模型）
struct SessionPolicy {
  int max_connections_per_user = 8;
  int heartbeat_timeout_ms = 30000;
  int round_robin_default_interval_ms = 100;
  int broadcast_max_recipients = 10000;
};

}  // namespace fwd
