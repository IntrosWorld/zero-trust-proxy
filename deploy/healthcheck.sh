#!/bin/bash
set -euo pipefail
exec 3<>"/dev/tcp/${ZT_ADMIN_HOST:-127.0.0.1}/${ZT_ADMIN_PORT:-9100}"
printf 'GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
IFS= read -r status <&3
[[ "$status" == 'HTTP/1.1 200 '* ]]
