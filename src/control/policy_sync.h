#pragma once
#include "admin/metrics.h"
#include "config/config.h"
#include <condition_variable>
#include <thread>
namespace zt {
class PolicySync {
  ConfigStore &store_;
  Metrics &metrics_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  void run();

public:
  PolicySync(ConfigStore &store, Metrics &metrics);
  ~PolicySync();
};
} // namespace zt
