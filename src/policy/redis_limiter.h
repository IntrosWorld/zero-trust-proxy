#pragma once
#include "admin/metrics.h"
#include "dns/resolver.h"
#include <condition_variable>
#include <deque>
#include <thread>
namespace zt {
extern const char *redis_bucket_script;
class RedisLimiter {
  struct Job {
    uint64_t id;
    std::string identity;
    std::shared_ptr<const Config> config;
    std::weak_ptr<Mailbox> mailbox;
    std::chrono::steady_clock::time_point queued;
  };
  Metrics &metrics_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
  void run();

public:
  RedisLimiter(int threads, Metrics &metrics);
  ~RedisLimiter();
  bool submit(uint64_t id, std::string identity, std::shared_ptr<const Config> config,
              const std::shared_ptr<Mailbox> &mailbox);
};
} // namespace zt
