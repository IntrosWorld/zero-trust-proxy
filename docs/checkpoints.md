# Verified checkpoints

- M1: CMake presets, RAII descriptors and epoll echo foundation. ASan/UBSan build and real socket echo smoke passed. The echo executable is replaced by the proxy in M3.
- M1 verification note: the first smoke attempt was sandbox-blocked; the subsequent approved run outside the sandbox passed without sanitizer diagnostics.
- M2: seven GoogleTest cases pass, covering every header split, bytewise CONNECT, framing ambiguity, limits, stripping, chunked and fixed bodies.
- M3–M9 implementation checkpoint: integrated the forwarding state machine with policy, DNS, SNI, worker isolation, authentication, limits, metrics and lifecycle together so no insecure forwarding-only binary is shipped. Sixteen unit cases and 21 real-socket integration cases pass. HTTP GET/POST/chunked, curl HTTP/HTTPS, TLS download SHA-256, fragmented ClientHello, SNI blocking, four workers, limits, reload and log checks are verified. Dedicated hardening and expanded scenario evidence follow in separate checkpoints. Initial concurrency failure was a five-entry Python backend backlog; increasing the fixture backlog to 256 made all clients succeed.
