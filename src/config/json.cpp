#include "config/json.h"
#include "log/access_log.h"
#include <iomanip>
#include <sstream>
namespace zt {
std::string json_dump(const Json &j) {
  if (std::holds_alternative<std::nullptr_t>(j.value))
    return "null";
  if (auto p = std::get_if<bool>(&j.value))
    return *p ? "true" : "false";
  if (auto p = std::get_if<double>(&j.value)) {
    std::ostringstream o;
    o << std::setprecision(17) << *p;
    return o.str();
  }
  if (auto p = std::get_if<std::string>(&j.value))
    return json_string(*p);
  std::string out;
  if (auto p = std::get_if<Json::Array>(&j.value)) {
    for (const auto &v : *p) {
      if (!out.empty())
        out += ',';
      out += json_dump(v);
    }
    return '[' + out + ']';
  }
  for (const auto &[k, v] : j.as<Json::Object>()) {
    if (!out.empty())
      out += ',';
    out += json_string(k) + ':' + json_dump(v);
  }
  return '{' + out + '}';
}
} // namespace zt
