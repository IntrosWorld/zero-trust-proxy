#pragma once
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
namespace zt {
struct Metrics {
  std::atomic<bool> redis_backend{false};
  std::atomic<uint64_t> redis_errors{0}, redis_us{0}, redis_fallback{0};
  std::array<std::atomic<uint64_t>, 9> redis_latency{};
  void observe_redis(uint64_t us);
  std::atomic<bool> ready{false};
  std::atomic<uint64_t> policy_reload_ok{0}, policy_reload_rejected{0}, policy_sync_errors{0};
  std::atomic<uint64_t> active{0}, connections{0}, bytes_up{0}, bytes_down{0}, log_drops{0},
      dns_count{0}, dns_us{0}, connect_count{0}, connect_us{0};
  std::array<std::atomic<uint64_t>, 9> latency{};
  std::atomic<uint64_t> latency_us{0};
  std::mutex mutex;
  std::map<std::string, uint64_t> rules, methods;
  void decision(const std::string &rule);
  void method(const std::string &value);
  void observe(double seconds);
  std::string render();
};
} // namespace zt
