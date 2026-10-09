FROM zt-policy:kubernetes
USER root
RUN apt-get update -o APT::Update::Error-Mode=any && apt-get install -y --no-install-recommends curl wrk && rm -rf /var/lib/apt/lists/*
COPY bench/k8s/client.py /opt/client.py
COPY bench/k8s/wrk.lua /opt/wrk.lua
USER 10001:10001
ENTRYPOINT ["sleep", "infinity"]
CMD []
