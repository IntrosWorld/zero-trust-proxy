#include "policy/rate_limit.h"
#include <algorithm>
namespace zt {
bool RateLimiter::allow(const std::string &ip, double rate, double burst, Clock::time_point now) {
  auto it = buckets_.find(ip);
  if (it == buckets_.end()) {
    if (buckets_.size() >= 65536)
      return false;
    it = buckets_.emplace(ip, Bucket{burst, burst / rate, now}).first;
  }
  auto &b = it->second;
  double elapsed = std::max(0.0, std::chrono::duration<double>(now - b.time).count());
  b.tokens = std::min(burst, b.tokens + elapsed * rate);
  b.time = std::max(now, b.time);
  b.refill_seconds = burst / rate;
  if (b.tokens < 1)
    return false;
  --b.tokens;
  return true;
}
void RateLimiter::prune(Clock::time_point now) {
  std::erase_if(buckets_, [&](const auto &p) {
    double idle = std::chrono::duration<double>(now - p.second.time).count();
    // Eviction grants a fresh burst on return. Only evict when an untouched
    // bucket would already be full, including very low configured rates.
    return idle > 600 && idle >= p.second.refill_seconds;
  });
}
} // namespace zt
