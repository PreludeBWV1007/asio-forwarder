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

}  // namespace demo

