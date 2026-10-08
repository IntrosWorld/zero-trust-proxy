#include "http/request_parser.h"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 65536)
    return 0;
  zt::RequestParser parser;
  auto bytes = std::string_view(reinterpret_cast<const char *>(data), size);
  size_t split = size ? data[0] % size : 0;
  parser.feed(bytes.substr(0, split));
  if (parser.feed(bytes.substr(split)) == zt::ParseStatus::complete) {
    (void)parser.request.forward_headers();
    zt::BodyFramer body(parser.request);
    (void)body.consume(parser.remainder);
  }
  return 0;
}
