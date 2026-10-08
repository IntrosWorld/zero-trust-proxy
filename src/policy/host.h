#pragma once
#include <string>
#include <vector>
namespace zt {
bool valid_host(const std::string &host);
bool suspicious_host(const std::string &host);
bool matches(const std::vector<std::string> &patterns, const std::string &host);
bool constant_time_equal(const std::string &a, const std::string &b);
} // namespace zt
