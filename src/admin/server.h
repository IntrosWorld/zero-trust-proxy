#pragma once
#include "admin/metrics.h"
#include "config/config.h"
#include "net/socket.h"
#include <atomic>
#include <thread>
namespace zt {
class AdminServer {
  Metrics &metrics_;
  ConfigStore &configs_;
  Fd listener_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  void run();

public:
  AdminServer(ConfigStore &configs, Metrics &metrics);
  ~AdminServer();
};
} // namespace zt
