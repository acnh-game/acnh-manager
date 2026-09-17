#!/usr/bin/env bash
# Build the NRO in the pinned image.  The docker socket is sandboxed, so this needs
# escalation.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
image="devkitpro/devkita64:20260219"

# Build stamp = UTC time + commit + source-tree hash.  It is written into the binary and
# shown in the log and in the UI footer, so "is this the build I just made?" has an answer:
# change any file under source/ and the stamp changes.
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
