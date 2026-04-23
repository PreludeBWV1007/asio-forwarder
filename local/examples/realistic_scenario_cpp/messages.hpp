#pragma once

#include <cstdint>
#include <string>

#include <msgpack.hpp>

namespace demo {

struct Task {
  std::uint64_t task_id{0};
  std::int64_t a{0};
  std::int64_t b{0};
  MSGPACK_DEFINE(task_id, a, b);
};

struct TaskResult {
  std::uint64_t task_id{0};
  bool ok{false};
  std::int64_t value{0};
  MSGPACK_DEFINE(task_id, ok, value);
};

struct Notice {
  std::string mode;
  std::string msg;
  std::uint64_t n{0};
  MSGPACK_DEFINE(mode, msg, n);
};

// 量化/券商推送常见的一类：逐笔/快照（简化版）。
// 字段控制在 10 个以内，便于跨语言与前端录入验证。
struct StockTick {
  std::string symbol;          // 例如 "600519.SH"
  std::uint64_t exchange_ts;   // 交易所时间戳（ms）
  double last{0};              // 最新价
  double open{0};              // 开盘价
  double high{0};              // 最高价
  double low{0};               // 最低价
  double prev_close{0};        // 昨收
  std::uint64_t volume{0};     // 成交量（股/手，按你券商定义）
  double turnover{0};          // 成交额
  bool is_trading{true};       // 是否在交易时段/是否可交易
  MSGPACK_DEFINE(symbol, exchange_ts, last, open, high, low, prev_close, volume, turnover, is_trading);
};

}  // namespace demo

