#!/usr/bin/env bash
# 在固定镜像里构建 NRO。Docker socket 受沙盒限制,运行前需提权。
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
image="devkitpro/devkita64:20260219"

exec docker run --rm --platform linux/amd64 \
    -v "${repo_dir}:/work" -w /work \
    "${image}" \
    bash -lc 'source /opt/devkitpro/switchvars.sh && make'
