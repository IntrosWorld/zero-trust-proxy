#pragma once
#include <chrono>
#include <unordered_map>
#include <string>
namespace zt {
using Clock=std::chrono::steady_clock;
class RateLimiter {
  struct Bucket { double tokens; Clock::time_point time; };
  std::unordered_map<std::string,Bucket> buckets_;
public:
  bool allow(const std::string& ip,double rate,double burst,Clock::time_point now=Clock::now());
  void prune(Clock::time_point now=Clock::now());
};
}
