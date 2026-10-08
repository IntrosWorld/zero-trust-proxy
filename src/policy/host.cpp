#include "policy/host.h"
#include "http/request_parser.h"
#include <algorithm>
namespace zt {
bool valid_host(const std::string &s) {
  if (s.empty() || s.size() > 253 || s.back() == '.')
    return false;
  size_t start = 0;
  do {
    auto end = s.find('.', start);
    if (end == s.npos)
      end = s.size();
    auto n = end - start;
    if (n == 0 || n > 63 || s[start] == '-' || s[end - 1] == '-')
      return false;
    for (size_t i = start; i < end; ++i) {
      char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
        return false;
    }
    start = end + 1;
  } while (start < s.size());
  return true;
}
bool suspicious_host(const std::string &s) {
  return s.starts_with("xn--") || s.find(".xn--") != s.npos || !valid_host(s);
}
bool matches(const std::vector<std::string> &patterns, const std::string &host) {
  for (const auto &p : patterns) {
    auto rule = lower(p);
    if (rule == host)
      return true;
    if (rule.starts_with("*.") && host.size() > rule.size() - 1 && host.ends_with(rule.substr(1)))
      return true;
  }
  return false;
}
bool constant_time_equal(const std::string &a, const std::string &b) {
  volatile unsigned int diff = static_cast<unsigned int>(a.size() ^ b.size());
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
    diff = diff | ((i < a.size() ? static_cast<unsigned char>(a[i]) : 0U) ^
                   (i < b.size() ? static_cast<unsigned char>(b[i]) : 0U));
  return diff == 0;
}
} // namespace zt
