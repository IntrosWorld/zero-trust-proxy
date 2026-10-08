#include "log/access_log.h"
#include <stdexcept>
namespace zt {
std::string json_string(const std::string &s) {
  std::string out = "\"";
  const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32 || c >= 127) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  return out + '"';
}
AccessLog::AccessLog(const std::string &path, Metrics &metrics)
    : metrics_(metrics), output_(path, std::ios::app) {
  if (!output_)
    throw std::runtime_error("cannot open access log: " + path);
  thread_ = std::thread([this] { run(); });
}
AccessLog::~AccessLog() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_one();
  thread_.join();
}
void AccessLog::push(std::string line) {
  {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= 8192) {
      metrics_.log_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    queue_.push_back(std::move(line));
  }
  cv_.notify_one();
}
void AccessLog::run() {
  for (;;) {
    std::deque<std::string> batch;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      batch.swap(queue_);
      if (batch.empty() && stop_)
        break;
    }
    for (const auto &line : batch) {
      output_ << line << '\n';
      if (!output_)
        metrics_.log_drops.fetch_add(1, std::memory_order_relaxed);
    }
    output_.flush();
  }
}
} // namespace zt
