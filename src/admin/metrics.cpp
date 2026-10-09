#include "admin/metrics.h"
#include <sstream>
namespace zt {
static constexpr double bounds[] = {.001, .005, .01, .05, .1, .5, 1, 5};
void Metrics::decision(const std::string &rule) {
  std::lock_guard lock(mutex);
  ++rules[rule];
}
void Metrics::method(const std::string &value) {
  std::lock_guard lock(mutex);
  static const std::string known[] = {"GET",  "POST",    "PUT",   "DELETE",
                                      "HEAD", "OPTIONS", "PATCH", "CONNECT"};
  for (const auto &k : known)
    if (k == value) {
      ++methods[k];
      return;
    }
  ++methods["OTHER"];
}
void Metrics::observe(double seconds) {
  latency_us.fetch_add(seconds * 1e6, std::memory_order_relaxed);
  size_t i = 0;
  while (i < 8 && seconds > bounds[i])
    ++i;
  latency[i].fetch_add(1, std::memory_order_relaxed);
}
void Metrics::observe_redis(uint64_t us) {
  redis_us.fetch_add(us);
  size_t i = 0;
  while (i < 8 && us / 1e6 > bounds[i])
    ++i;
  ++redis_latency[i];
}
std::string Metrics::render() {
  std::ostringstream o;
  auto emit = [&](const char *n, const char *type, uint64_t v) {
    o << "# TYPE zt_" << n << ' ' << type << "\nzt_" << n << ' ' << v << '\n';
  };
  emit("active_connections", "gauge", active.load(std::memory_order_relaxed));
  emit("connections_total", "counter", connections.load(std::memory_order_relaxed));
  emit("bytes_up_total", "counter", bytes_up.load(std::memory_order_relaxed));
  emit("bytes_down_total", "counter", bytes_down.load(std::memory_order_relaxed));
  emit("log_drops_total", "counter", log_drops.load(std::memory_order_relaxed));
  {
    std::lock_guard lock(mutex);
    o << "# TYPE zt_decisions_total counter\n";
    for (const auto &[rule, n] : rules)
      o << "zt_decisions_total{rule=\"" << rule << "\"} " << n << '\n';
    o << "# TYPE zt_requests_total counter\n";
    for (const auto &[method, n] : methods)
      o << "zt_requests_total{method=\"" << method << "\"} " << n << '\n';
  }
  o << "# TYPE zt_request_duration_seconds histogram\n";
  uint64_t total = 0;
  for (size_t i = 0; i < 9; ++i) {
    total += latency[i].load(std::memory_order_relaxed);
    o << "zt_request_duration_seconds_bucket{le=\"";
    if (i < 8)
      o << bounds[i];
    else
      o << "+Inf";
    o << "\"} " << total << '\n';
  }
  o << "zt_request_duration_seconds_count " << total << "\nzt_request_duration_seconds_sum "
    << latency_us.load(std::memory_order_relaxed) / 1e6 << '\n';
  o << "# TYPE zt_dns_duration_seconds summary\nzt_dns_duration_seconds_count " << dns_count.load()
    << "\nzt_dns_duration_seconds_sum " << dns_us.load() / 1e6 << '\n';
  o << "# TYPE zt_connect_duration_seconds summary\nzt_connect_duration_seconds_count "
    << connect_count.load() << "\nzt_connect_duration_seconds_sum " << connect_us.load() / 1e6
    << '\n';
  o << "# TYPE zt_policy_reload_total counter\nzt_policy_reload_total{result=\"ok\"} "
    << policy_reload_ok.load() << "\nzt_policy_reload_total{result=\"rejected\"} "
    << policy_reload_rejected.load() << '\n';
  emit("policy_sync_errors_total", "counter", policy_sync_errors.load());
  emit("redis_errors_total", "counter", redis_errors.load());
  emit("ratelimit_fallback_total", "counter", redis_fallback.load());
  o << "# TYPE zt_ratelimit_backend gauge\nzt_ratelimit_backend{backend=\"redis\"} "
    << (redis_backend.load() ? 1 : 0) << "\nzt_ratelimit_backend{backend=\"local\"} "
    << (redis_backend.load() ? 0 : 1) << '\n';
  uint64_t redis_total = 0;
  o << "# TYPE zt_redis_duration_seconds histogram\n";
  for (size_t i = 0; i < 9; ++i) {
    redis_total += redis_latency[i].load();
    o << "zt_redis_duration_seconds_bucket{le=\"";
    if (i < 8)
      o << bounds[i];
    else
      o << "+Inf";
    o << "\"} " << redis_total << '\n';
  }
  o << "zt_redis_duration_seconds_count " << redis_total << "\nzt_redis_duration_seconds_sum "
    << redis_us.load() / 1e6 << '\n';
  return o.str();
}
} // namespace zt
