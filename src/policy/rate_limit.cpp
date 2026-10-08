#include "policy/rate_limit.h"
#include <algorithm>
namespace zt {
bool RateLimiter::allow(const std::string &ip, double rate, double burst, Clock::time_point now) {
  auto it = buckets_.find(ip);
  if (it == buckets_.end()) {
    if (buckets_.size() >= 65536)
      return false;
    it = buckets_.emplace(ip, Bucket{burst, now}).first;
  }
  auto &b = it->second;
  double elapsed = std::max(0.0, std::chrono::duration<double>(now - b.time).count());
  b.tokens = std::min(burst, b.tokens + elapsed * rate);
  b.time = std::max(now, b.time);
  if (b.tokens < 1)
    return false;
  --b.tokens;
  return true;
}
void RateLimiter::prune(Clock::time_point now) {
  std::erase_if(buckets_,
                [&](const auto &p) { return now - p.second.time > std::chrono::minutes(10); });
}
} // namespace zt
