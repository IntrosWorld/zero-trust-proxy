#pragma once
#include "config/config.h"
#include "net/addr.h"
#include "net/socket.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
namespace zt {
struct DnsResult {
  uint64_t id;
  std::vector<Address> addresses;
  double seconds = 0;
};
struct RateResult {
  uint64_t id;
  bool available;
  bool allowed;
};
struct Mailbox {
  Fd wakeup;
  std::mutex mutex;
  std::deque<DnsResult> results;
  std::deque<RateResult> rate_results;
  Mailbox();
};
class Resolver {
  struct Job {
    uint64_t id;
    std::string host;
    int port;
    std::shared_ptr<const Config> config;
    std::weak_ptr<Mailbox> box;
  };
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
  void run();

public:
  explicit Resolver(int threads);
  ~Resolver();
  bool submit(uint64_t id, const std::string &host, int port, std::shared_ptr<const Config> config,
              const std::shared_ptr<Mailbox> &box);
};
} // namespace zt
