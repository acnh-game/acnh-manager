#!/usr/bin/env bash
# Build tools/ncz-probe.cpp against this repository's installer code and run it on a package.
#
#   tools/ncz-probe.sh <container> [entry index]
#
# The probe is host C++ (no libnx, no Docker): container.*, ncz.* and ncz_decode.* are the modules
# the console build uses, so what it prints is what the install would decode.  zstd comes from the
# host (brew); the console build links libnx's copy of the same library.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${repo_dir}/build/scratch"
mkdir -p "${out_dir}"
probe="${out_dir}/ncz-probe"

c++ -std=c++17 -O2 -Wall -Wextra -I "${repo_dir}/source" \
    "${repo_dir}/tools/ncz-probe.cpp" \
    "${repo_dir}/source/install/container.cpp" \
    "${repo_dir}/source/install/ncz.cpp" \
    "${repo_dir}/source/install/ncz_decode.cpp" \
    "${repo_dir}/source/util/sha256.cpp" \
    -o "${probe}" -lzstd

exec "${probe}" "$@"
