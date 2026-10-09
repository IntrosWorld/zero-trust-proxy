#include "policy/resp.h"
#include <charconv>
#include <stdexcept>
namespace zt {
std::string resp_command(const std::vector<std::string> &args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto &arg : args)
    out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
  return out;
}
namespace {
struct Incomplete {};
Resp parse(std::string_view data, size_t &p, int depth) {
  if (depth > 8)
    throw std::runtime_error("RESP nesting limit");
  if (p >= data.size())
    throw Incomplete{};
  Resp r;
  r.type = data[p++];
  auto line = data.find("\r\n", p);
  if (line == data.npos) {
    if (data.size() - p > 65536)
      throw std::runtime_error("RESP line limit");
    throw Incomplete{};
  }
  auto text = data.substr(p, line - p);
  p = line + 2;
  if (r.type == '+' || r.type == '-') {
    r.text = text;
    return r;
  }
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), r.number);
  if (ec != std::errc() || end != text.data() + text.size())
    throw std::runtime_error("invalid RESP integer");
  if (r.type == ':')
    return r;
  if (r.type != '$' && r.type != '*')
    throw std::runtime_error("invalid RESP type");
  if (r.number == -1)
    return r;
  if (r.number < 0 || r.number > 1024 * 1024)
    throw std::runtime_error("RESP size limit");
  if (r.type == '$') {
    auto n = static_cast<size_t>(r.number);
    if (data.size() - p < n + 2)
      throw Incomplete{};
    if (data.substr(p + n, 2) != "\r\n")
      throw std::runtime_error("invalid RESP bulk ending");
    r.text = data.substr(p, n);
    p += n + 2;
  } else {
    if (r.number > 128)
      throw std::runtime_error("RESP array limit");
    for (int64_t i = 0; i < r.number; ++i)
      r.elements.push_back(parse(data, p, depth + 1));
  }
  return r;
}
} // namespace
std::optional<Resp> resp_parse(std::string_view data, size_t &consumed) {
  if (data.size() > 1024 * 1024 + 65536)
    throw std::runtime_error("RESP response limit");
  size_t p = 0;
  consumed = 0;
  try {
    auto r = parse(data, p, 0);
    consumed = p;
    return r;
  } catch (const Incomplete &) {
    return {};
  }
}
} // namespace zt
