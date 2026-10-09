#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$ZT_ROOT"
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts
helm repo update prometheus-community
helm upgrade --install monitoring prometheus-community/kube-prometheus-stack --version 92.2.0 \
  --namespace monitoring --create-namespace -f deploy/k8s/monitoring-values.yaml --wait --timeout 10m
kubectl -n monitoring create configmap zt-dashboard --from-file=zt-proxy.json=deploy/grafana/zt-proxy-dashboard.json --dry-run=client -o yaml | kubectl apply -f -
kubectl -n monitoring label configmap zt-dashboard grafana_dashboard=1 --overwrite
helm upgrade zt-proxy deploy/helm/zt-proxy --namespace zt-system --reuse-values --set monitoring.enabled=true --wait --timeout 3m
