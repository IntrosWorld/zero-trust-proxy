# Verified checkpoints

- M1: CMake presets, RAII descriptors and epoll echo foundation. ASan/UBSan build and real socket echo smoke passed. The echo executable is replaced by the proxy in M3.
- M1 verification note: the first smoke attempt was sandbox-blocked; the subsequent approved run outside the sandbox passed without sanitizer diagnostics.
- M2: seven GoogleTest cases pass, covering every header split, bytewise CONNECT, framing ambiguity, limits, stripping, chunked and fixed bodies.
