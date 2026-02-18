#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tr {

inline std::string now_ts() {
  using namespace std::chrono;
  const auto tp = system_clock::now();
  const auto t = system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

inline void log_info(const std::string& msg) {
  std::cerr << "[" << now_ts() << "] [INFO] " << msg << "\n";
}
inline void log_warn(const std::string& msg) {
  std::cerr << "[" << now_ts() << "] [WARN] " << msg << "\n";
}
inline void log_err(const std::string& msg) {
  std::cerr << "[" << now_ts() << "] [ERR ] " << msg << "\n";
}

inline uint64_t steady_millis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline uint64_t next_corr_id() {
  static std::atomic<uint64_t> g{1};
  return g.fetch_add(1, std::memory_order_relaxed);
}

} // namespace tr
