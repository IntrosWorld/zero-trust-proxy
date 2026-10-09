#include "policy/redis_limiter.h"
#include "control/transport.h"
#include "policy/resp.h"
#include <iomanip>
#include <sstream>
#include <unistd.h>
namespace zt {
// Redis TIME supplies one clock for every replica; the script is one atomic transaction.
const char *redis_bucket_script = R"lua(
local rate=tonumber(ARGV[1])
local burst=tonumber(ARGV[2])
local nowparts=redis.call('TIME')
local now=tonumber(nowparts[1])+tonumber(nowparts[2])/1000000
local old=redis.call('HMGET',KEYS[1],'tokens','time')
local tokens=tonumber(old[1]) or burst
local last=tonumber(old[2]) or now
now=math.max(now,last)
tokens=math.min(burst,tokens+math.max(0,now-last)*rate)
local allowed=0
if tokens>=1 then tokens=tokens-1; allowed=1 end
redis.call('HSET',KEYS[1],'tokens',tokens,'time',now)
local ttl=math.max(600000,math.ceil(burst/rate*1000))
if ttl<9007199254740000 then redis.call('PEXPIRE',KEYS[1],ttl) else redis.call('PERSIST',KEYS[1]) end
return allowed
)lua";
RedisLimiter::RedisLimiter(int threads, Metrics &metrics) : metrics_(metrics) {
  for (int i = 0; i < threads; ++i)
    threads_.emplace_back([this] { run(); });
}
RedisLimiter::~RedisLimiter() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  for (auto &thread : threads_)
    thread.join();
}
bool RedisLimiter::submit(uint64_t id, std::string identity, std::shared_ptr<const Config> config,
                          const std::shared_ptr<Mailbox> &mailbox) {
  {
    std::lock_guard lock(mutex_);
    if (stop_ || jobs_.size() >= 4096)
      return false;
    jobs_.push_back(
        {id, std::move(identity), std::move(config), mailbox, std::chrono::steady_clock::now()});
  }
  cv_.notify_one();
  return true;
}
void RedisLimiter::run() {
  std::unique_ptr<Stream> stream;
  std::string buffer, sha;
  auto retry = std::chrono::steady_clock::time_point{};
  int backoff = 100;
  while (true) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_)
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    RateResult result{job.id, false, false};
    auto start = std::chrono::steady_clock::now();
    try {
      if (start < retry ||
          start - job.queued > std::chrono::milliseconds(job.config->redis_timeout_ms * 3))
        throw std::runtime_error("Redis backoff/queue deadline");
      auto call = [&](const std::vector<std::string> &args) {
        stream->deadline(job.config->redis_timeout_ms);
        stream->write(resp_command(args));
        for (;;) {
          size_t used = 0;
          auto parsed = resp_parse(buffer, used);
          if (parsed) {
            buffer.erase(0, used);
            return *parsed;
          }
          auto chunk = stream->read();
          if (chunk.empty())
            throw std::runtime_error("Redis EOF");
          buffer += chunk;
        }
      };
      if (!stream) {
        stream = std::make_unique<Stream>(job.config->redis_host, job.config->redis_port,
                                          job.config->redis_timeout_ms);
        buffer.clear();
        sha.clear();
        if (!job.config->redis_password_file.empty()) {
          auto auth = call({"AUTH", read_secret(job.config->redis_password_file)});
          if (auth.type != '+' || auth.text != "OK")
            throw std::runtime_error("Redis authentication failed");
        }
      }
      auto load = [&] {
        auto r = call({"SCRIPT", "LOAD", redis_bucket_script});
        if (r.type != '$' || r.text.size() != 40)
          throw std::runtime_error("Redis script load failed");
        sha = r.text;
      };
      if (sha.empty())
        load();
      auto decimal = [](double v) {
        std::ostringstream out;
        out << std::setprecision(17) << v;
        return out.str();
      };
      auto args = std::vector<std::string>{"EVALSHA",
                                           sha,
                                           "1",
                                           "zt:bucket:" + job.identity,
                                           decimal(job.config->rate),
                                           decimal(job.config->burst)};
      auto response = call(args);
      if (response.type == '-' && response.text.starts_with("NOSCRIPT")) {
        load();
        args[1] = sha;
        response = call(args);
      }
      if (response.type != ':' || (response.number != 0 && response.number != 1))
        throw std::runtime_error("Redis bucket failed");
      result.available = true;
      result.allowed = response.number == 1;
      backoff = 100;
      retry = {};
    } catch (const std::exception &) {
      stream.reset();
      buffer.clear();
      sha.clear();
      ++metrics_.redis_errors;
      if (start >= retry) {
        retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff);
        backoff = std::min(backoff * 2, 2000);
      }
    }
    metrics_.redis_backend.store(result.available);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    metrics_.observe_redis(us);
    if (auto mailbox = job.mailbox.lock()) {
      {
        std::lock_guard lock(mailbox->mutex);
        if (mailbox->rate_results.size() >= 8192)
          continue;
        mailbox->rate_results.push_back(result);
      }
      uint64_t one = 1;
      auto ignored = write(mailbox->wakeup.get(), &one, sizeof(one));
      (void)ignored;
    }
  }
}
} // namespace zt
