ARG PROXY_IMAGE=zt-proxy:kubernetes
FROM ${PROXY_IMAGE}
USER root
RUN apt-get update -o APT::Update::Error-Mode=any && apt-get install -y --no-install-recommends python3 && rm -rf /var/lib/apt/lists/* && mkdir -p /data && chown 10001:10001 /data
COPY control-plane/server.py /opt/zt-policy/server.py
USER 10001:10001
WORKDIR /data
EXPOSE 8080
HEALTHCHECK NONE
ENTRYPOINT ["python3", "/opt/zt-policy/server.py"]
CMD []
