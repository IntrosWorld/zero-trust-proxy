#pragma once
#include "net/socket.h"
#include <atomic>
#include <chrono>
#include <map>
#include <string>
namespace zt {
class Stream {
  Fd fd_;
  std::chrono::steady_clock::time_point deadline_;
  const std::atomic<bool> *cancel_;
  void wait(short events);

public:
  Stream(const std::string &host, int port, int timeout_ms,
         const std::atomic<bool> *cancel = nullptr);
  void deadline(int timeout_ms);
  void write(const std::string &data);
  std::string read();
};
struct HttpResponse {
  int status;
  std::map<std::string, std::string> headers;
  std::string body;
};
HttpResponse parse_response(const std::string &raw);
HttpResponse http_request(const std::string &url, const std::string &method,
                          const std::string &token, const std::string &etag,
                          const std::string &body, int timeout_ms,
                          const std::atomic<bool> *cancel = nullptr);
} // namespace zt
