#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace zt {
struct Resp {
  char type = 0;
  std::string text;
  int64_t number = 0;
  std::vector<Resp> elements;
};
std::string resp_command(const std::vector<std::string> &args);
std::optional<Resp> resp_parse(std::string_view data, size_t &consumed);
} // namespace zt
