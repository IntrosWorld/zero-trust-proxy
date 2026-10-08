#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace zt {
struct Request {
  std::string method, host, path, authorization;
  int port = 80;
  bool tunnel = false, chunked = false;
  uint64_t content_length = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string forward_headers() const;
};
enum class ParseStatus { need_more, complete, error };
class RequestParser {
  std::string data_;
  ParseStatus status_ = ParseStatus::need_more;
  void parse();

public:
  Request request;
  int error_code = 400;
  std::string remainder;
  ParseStatus feed(std::string_view bytes);
};
std::string lower(std::string_view value);
// Validates a single request body incrementally; never forwards pipelined bytes.
class BodyFramer {
  enum class State { size, data, cr, lf, trailers, done };
  State state_ = State::size;
  uint64_t left_ = 0;
  bool chunked_;
  std::string line_;
  size_t trailer_bytes_ = 0;

public:
  explicit BodyFramer(const Request &request);
  bool consume(std::string_view bytes); // false on invalid framing or surplus bytes
  bool done() const {
    return state_ == State::done;
  }
};
} // namespace zt
