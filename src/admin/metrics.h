#pragma once
#include <array>
#include <atomic>
#include <string>
#include <mutex>
#include <map>
namespace zt {
struct Metrics {
  std::atomic<uint64_t> active{0},connections{0},bytes_up{0},bytes_down{0},log_drops{0},dns_count{0},dns_us{0},connect_count{0},connect_us{0};
  std::array<std::atomic<uint64_t>,9> latency{};
  std::atomic<uint64_t> latency_us{0};
  std::mutex mutex;
  std::map<std::string,uint64_t> rules,methods;
  void decision(const std::string& rule);
  void method(const std::string& value);
  void observe(double seconds);
  std::string render();
};
}
