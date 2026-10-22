#!/usr/bin/env bash
# Build the NRO in the pinned image.  The docker socket is sandboxed, so this needs
# escalation.
#
# Development aids (the probes in source/probe.cpp and the console text-UI fallback) are ON by
# default here, because this is the development loop.  `--release` turns them off, which is what
# tools/release.sh uses: a released binary must not carry a probe that writes a 4.5 GB file when a
# file with a magic name exists.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
image="devkitpro/devkita64:20260219"
dev_aids=1
for arg in "$@"; do
    case "${arg}" in
        # A release build ships with the probes switched off.  Whether it writes log.txt is not a
        # build decision at all: the app asks the card at startup (see source/log.hpp), so a
        # released binary can be told to log by dropping `log-on` next to it.
        --release) dev_aids=0 ;;
        *) echo "unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done

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
# Everything linked into the binary or required to certify its embedded assets: `source`, `data`
# and `assets`.  The forwarder metadata is not linked, but it is part of that certification.
# Hashing only `source` left the stamp unchanged when the web page changed -- and the stamp is what
# the page's version chip and the console footer use to answer "is this tab older than the console?"
# (2026-10-17: a page-only fix kept the same chip, so the stale-tab check could not see it).
src_hash="$(cd "${repo_dir}" && find source data assets -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-8)"
stamp="${stamp}+src:${src_hash}"
if [ "${dev_aids}" -eq 1 ]; then
    stamp="${stamp}+devaids"
else
    # Say it on the tin: the footer and log.txt then answer "which kind of build is this?" --
    # a release build carries no probes, and the stamp is where that is visible.
    stamp="${stamp}+release"
fi
echo "build stamp: ${stamp}"

exec docker run --rm --platform linux/amd64 \
    -v "${repo_dir}:/work" -w /work \
    -e "ACNH_BUILD_STAMP=${stamp}" \
    "${image}" \
    bash -lc "source /opt/devkitpro/switchvars.sh && make BUILD_STAMP=\"\$ACNH_BUILD_STAMP\" DEV_AIDS=${dev_aids}"
