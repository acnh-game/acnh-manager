#!/usr/bin/env bash
# 在固定镜像里构建 NRO。Docker socket 受沙盒限制,运行前需提权。
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
image="devkitpro/devkita64:20260219"

# 构建戳:UTC 时间 + commit + **源码树哈希**。写进二进制并在日志/界面上显示,
# 用来回答"现在跑的是不是最新构建":源码一变,戳必变。
stamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
if commit="$(git -C "${repo_dir}" rev-parse --short HEAD 2>/dev/null)"; then
    stamp="${stamp}+${commit}"
    if [ -n "$(git -C "${repo_dir}" status --porcelain 2>/dev/null)" ]; then
        stamp="${stamp}-dirty"
    fi
fi
src_hash="$(cd "${repo_dir}" && find source -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-8)"
stamp="${stamp}+src:${src_hash}"
echo "build stamp: ${stamp}"

exec docker run --rm --platform linux/amd64 \
    -v "${repo_dir}:/work" -w /work \
    -e "ACNH_BUILD_STAMP=${stamp}" \
    "${image}" \
    bash -lc 'source /opt/devkitpro/switchvars.sh && make BUILD_STAMP="$ACNH_BUILD_STAMP"'
