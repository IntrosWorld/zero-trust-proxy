# Ubuntu's minimal image has no CA bundle. Bootstrap HTTPS from the official
# Alpine image, then install Ubuntu's maintained ca-certificates package.
FROM alpine:3.22 AS certificates
FROM ubuntu:22.04 AS base
COPY --from=certificates /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://|https://|g' /etc/apt/sources.list

FROM base AS build
RUN apt-get update -o APT::Update::Error-Mode=any && apt-get install -y --no-install-recommends build-essential cmake libssl-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt CMakePresets.json ./
COPY src/ src/
RUN cmake --preset release -DBUILD_TESTING=OFF && cmake --build --preset release

FROM base AS runtime
RUN apt-get update -o APT::Update::Error-Mode=any && apt-get install -y --no-install-recommends libstdc++6 libssl3 ca-certificates && rm -rf /var/lib/apt/lists/* && groupadd --system --gid 10001 proxyuser && useradd --system --uid 10001 --gid 10001 --home /var/lib/zt-proxy proxyuser && mkdir -p /var/lib/zt-proxy /etc/zt-proxy && chown 10001:10001 /var/lib/zt-proxy
COPY --from=build /source/build/release/zt-proxy /usr/local/bin/zt-proxy
COPY config/zt-proxy.json /etc/zt-proxy/config.json
COPY --chmod=755 deploy/healthcheck.sh /usr/local/bin/zt-healthcheck
LABEL org.opencontainers.image.title="zt-proxy" org.opencontainers.image.description="C++20 zero trust forward proxy"
USER 10001:10001
WORKDIR /var/lib/zt-proxy
EXPOSE 3128
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 CMD ["/usr/local/bin/zt-healthcheck"]
ENTRYPOINT ["/usr/local/bin/zt-proxy"]
CMD ["--config", "/etc/zt-proxy/config.json"]
