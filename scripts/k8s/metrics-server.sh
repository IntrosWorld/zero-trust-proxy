#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
kubectl apply -f https://github.com/kubernetes-sigs/metrics-server/releases/download/v0.9.0/components.yaml
# Local kind kubelet certificates do not have trusted IP SANs. Do not use this flag in production.
kubectl -n kube-system patch deployment metrics-server --type=json -p='[{"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"--kubelet-insecure-tls"}]'
kubectl -n kube-system rollout status deployment/metrics-server --timeout=180s
