#include "http/request_parser.h"
#include <chrono>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  int iterations = argc > 1 ? std::stoi(argv[1]) : 4096;
  std::string block(65536, 'x');
  zt::Request request;
  request.content_length = static_cast<uint64_t>(iterations) * block.size();
  zt::BodyFramer body(request);
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i)
    if (!body.consume(block))
      return 1;
  auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!body.done())
    return 2;
  std::cout << "{\"bytes\":" << request.content_length << ",\"seconds\":" << seconds
            << ",\"mib_per_second\":" << request.content_length / 1048576.0 / seconds << "}\n";
}
