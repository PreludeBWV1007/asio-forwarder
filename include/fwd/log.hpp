#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace fwd::log {

enum class Level { kInfo, kWarn, kError, kDebug };

inline const char* level_name(Level lv) {
  switch (lv) {
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
    case Level::kDebug:
      return "DEBUG";
  }
  return "INFO";
}

inline std::string now_local() {
  using namespace std::chrono;
  const auto tp = system_clock::now();
  const auto t = system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);
  const auto ms = duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms;
  return oss.str();
}

inline std::mutex& mu() {
  static std::mutex m;
  return m;
}

inline void write(Level lv, const std::string& msg) {
  std::lock_guard lk(mu());
  std::cerr << now_local() << " [" << level_name(lv) << "] " << msg << "\n";
}

}  // namespace fwd::log

