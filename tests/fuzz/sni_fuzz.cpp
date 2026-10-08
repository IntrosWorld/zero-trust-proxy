#include "tls/sni.h"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  (void)zt::extract_sni(std::string_view(reinterpret_cast<const char *>(data), size));
  return 0;
}
