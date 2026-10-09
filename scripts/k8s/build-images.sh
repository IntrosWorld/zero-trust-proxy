#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$ZT_ROOT"
docker build -f deploy/Dockerfile -t zt-proxy:kubernetes .
docker build -f control-plane/Dockerfile -t zt-policy:kubernetes .
docker build -f bench/k8s/Dockerfile -t zt-client:kubernetes .
docker tag zt-proxy:kubernetes zt-proxy:kubernetes-next
docker tag zt-policy:kubernetes zt-policy:kubernetes-next
kind load docker-image zt-proxy:kubernetes zt-proxy:kubernetes-next zt-policy:kubernetes zt-policy:kubernetes-next zt-client:kubernetes --name "$ZT_CLUSTER"
