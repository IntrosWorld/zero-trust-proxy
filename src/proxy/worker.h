#pragma once
#include "admin/metrics.h"
#include "dns/resolver.h"
#include "log/access_log.h"
#include "net/event_loop.h"
#include "policy/redis_limiter.h"
#include "proxy/connection.h"
#include <atomic>
#include <thread>
#include <unordered_map>
namespace zt {
class Worker {
  ConfigStore &configs_;
  Resolver &resolver_;
  Metrics &metrics_;
  AccessLog &log_;
  Fd listener_, timer_;
  EventLoop loop_;
  std::shared_ptr<Mailbox> mailbox_;
  std::unordered_map<uint64_t, std::unique_ptr<Connection>> connections_;
  std::unordered_map<std::string, size_t> per_client_;
  RateLimiter limiter_;
  RedisLimiter *redis_;
  uint64_t next_id_ = 8;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  Clock::time_point stop_time_{};
  void run();
  void accept_clients();
  void tick();
  void dns_results();
  void pump(Connection &c);
  void read_client(Connection &c);
  void read_upstream(Connection &c);
  void flush(Connection &c, bool upstream);
  void request_ready(Connection &c);
  void rate_result(Connection &c, bool available, bool allowed);
  void policy_ready(Connection &c);
  void connect_next(Connection &c);
  void connected(Connection &c);
  void hello_ready(Connection &c);
  void fail(Connection &c, int status, const std::string &rule, const std::string &reason);
  void arm(Connection &c);
  void reap();
  void record(Connection &c);

public:
  Worker(ConfigStore &configs, Resolver &resolver, Metrics &metrics, AccessLog &log,
         RedisLimiter *redis = nullptr);
  ~Worker();
  void start();
  void stop();
  void join();
};
} // namespace zt
