#pragma once
#include "admin/metrics.h"
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
namespace zt {
std::string json_string(const std::string &s);
class AccessLog {
  Metrics &metrics_;
  std::ofstream file_;
  std::ostream *output_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool stop_ = false;
  std::thread thread_;
  void run();

public:
  AccessLog(const std::string &path, Metrics &metrics);
  ~AccessLog();
  void push(std::string line);
};
} // namespace zt
