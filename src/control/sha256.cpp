#include "control/sha256.h"
#include <openssl/sha.h>
namespace zt {
std::string sha256(std::string_view input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto c : digest) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}
} // namespace zt
