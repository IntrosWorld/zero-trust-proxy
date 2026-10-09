#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
mkdir -p "$ZT_ROOT/artifacts/bin" "$ZT_ROOT/artifacts/downloads"
cd "$ZT_ROOT/artifacts/downloads"
curl -fL --retry 3 -o kind https://kind.sigs.k8s.io/dl/v0.33.0/kind-linux-amd64
curl -fL --retry 3 -o kind.sha256 https://kind.sigs.k8s.io/dl/v0.33.0/kind-linux-amd64.sha256sum
printf '%s  kind\n' "$(cut -d ' ' -f1 kind.sha256)" | sha256sum -c -
install -m755 kind ../bin/kind
curl -fL --retry 3 -o kubectl https://dl.k8s.io/release/v1.36.4/bin/linux/amd64/kubectl
curl -fL --retry 3 -o kubectl.sha256 https://dl.k8s.io/release/v1.36.4/bin/linux/amd64/kubectl.sha256
printf '%s  kubectl\n' "$(cat kubectl.sha256)" | sha256sum -c -
install -m755 kubectl ../bin/kubectl
curl -fL --retry 3 -o helm.tar.gz https://get.helm.sh/helm-v4.3.0-linux-amd64.tar.gz
curl -fL --retry 3 -o helm.sha256 https://get.helm.sh/helm-v4.3.0-linux-amd64.tar.gz.sha256sum
printf '%s  helm.tar.gz\n' "$(cut -d ' ' -f1 helm.sha256)" | sha256sum -c -
tar -xzf helm.tar.gz linux-amd64/helm
install -m755 linux-amd64/helm ../bin/helm
kind version
kubectl version --client
helm version
