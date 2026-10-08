#pragma once
#include <string>
#include <string_view>
namespace zt {
enum class SniStatus { ok, no_sni, need_more_data, not_tls, malformed };
struct SniResult {
  SniStatus status;
  std::string hostname;
};
SniResult extract_sni(std::string_view bytes);
} // namespace zt
