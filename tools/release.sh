#!/usr/bin/env bash
# ACNH-Manager release chain.  Everything runs from this repo, and the acnh-agent
# artifacts it needs are built here on the spot.  The agent repo is mounted read-only and
# its own dist/ is never read or written: the build happens in a throwaway copy inside the
# container.
#
#   1) build the acnh-agent release form in the pinned image (make switch SEMANTIC_HOOK=1)
#   2) derive main.npdm with acnh-agent's make-minimal-npdm.py from the original NPDM
#   3) release gate + import -> packaging/agent-lock.json, packaging/agent/<ver>/, data/
#   4) build the NRO (data/ is embedded into .rodata)
#   5) verify: lock <-> release record <-> data/ <-> what is inside the NRO
#   6) build the store package
#
# Usage:
#   ./tools/release.sh                     # whole chain
#   ./tools/release.sh --skip-agent-build  # reuse the stage in build/scratch/agent-release
#   ./tools/release.sh --skip-nro --skip-store
#   ./tools/release.sh --allow-dirty       # development only: validate the chain itself
#
# Needs docker (pinned image devkitpro/devkita64:20260219); inside the sandbox docker needs
# escalation.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
agent_dir="${ACNH_AGENT_DIR:-$(cd "${repo_dir}/../acnh-agent" && pwd)}"
image="devkitpro/devkita64:20260219"
stage="${repo_dir}/build/scratch/agent-release"
# Original ACNH main.npdm -- the only input of the NPDM derivation; acnh-agent keeps it
# under research/.
original_npdm="${agent_dir}/research/acnh-3.0.3/exefs/_ 3.0.3 [01006F8002326800][v2228224][UPD]/Program #0/0/main.npdm"

skip_agent_build=0
skip_nro=0
skip_store=0
allow_dirty=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-agent-build) skip_agent_build=1 ;;
        --skip-nro)         skip_nro=1 ;;
        --skip-store)       skip_store=1 ;;
        --allow-dirty)      allow_dirty=1 ;;
        -h|--help)          sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { echo; echo "=== $* ==="; }

step "0/6 preflight"
[ -d "${agent_dir}" ] || { echo "error: acnh-agent not found at ${agent_dir} (override with ACNH_AGENT_DIR)" >&2; exit 2; }
# This repo has to be clean as well: the build stamp carries its commit and source hash,
# and a dirty tree would stamp the NRO with -dirty.
if [ -n "$(git -C "${repo_dir}" status --porcelain)" ]; then
    if [ "${allow_dirty}" -eq 1 ]; then
        echo "warning: acnh-manager working tree is dirty (--allow-dirty): development validation only, not a release." >&2
    else
        echo "error: acnh-manager working tree is dirty; a release must come from committed sources." >&2
        echo "       (use --allow-dirty to validate the chain itself; that NRO stamp will say -dirty)" >&2
        exit 2
    fi
fi
head_commit="$(git -C "${agent_dir}" rev-parse --short=12 HEAD)"
if [ -n "$(git -C "${agent_dir}" status --porcelain)" ]; then
    echo "error: acnh-agent working tree is dirty; a release must come from a clean commit (otherwise version.json carries dirty=true and the gate refuses it)" >&2
    exit 2
fi
[ -f "${original_npdm}" ] || { echo "error: original main.npdm not found: ${original_npdm}" >&2; exit 2; }
echo "acnh-agent: ${agent_dir} @ ${head_commit} (clean tree)"

if [ "${skip_agent_build}" -eq 0 ]; then
    step "1/6 build the acnh-agent release form in the container (SEMANTIC_HOOK=1)"
    command -v docker >/dev/null || { echo "error: docker is required" >&2; exit 2; }
    rm -rf "${stage}"
    mkdir -p "${stage}"
    echo "the agent repo is mounted read-only; the container copies it to /work and builds there,"
    echo "so its dist/ is never touched.  The copy target is /work (not /work/agent): source"
    echo "paths end up in read-only data, and pinning them is what makes one commit build the same bytes."
    docker run --rm --platform linux/amd64 \
        -v "${agent_dir}:/agent:ro" -v "${stage}:/stage" \
        "${image}" \
        bash -lc 'set -euo pipefail
            rm -rf /work && mkdir -p /work
            (cd /agent && tar --exclude=./research --exclude=./dist --exclude=./build -cf - .) \
                | (cd /work && tar -xf -)
            cd /work
            source /opt/devkitpro/switchvars.sh
            make switch SEMANTIC_HOOK=1
            cp dist/acnh-agent.nso /stage/acnh-agent.nso
            cp dist/version.json /stage/version.json
            git rev-parse --short=12 HEAD > /stage/commit.txt'
else
    step "1/6 skipping the agent build (reusing ${stage})"
    [ -f "${stage}/acnh-agent.nso" ] || { echo "error: ${stage} has no artifact from a previous build" >&2; exit 2; }
fi

step "1b/6 the staged artifact must come from the current agent HEAD"
staged_commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "${stage}/version.json")"
if [ "${staged_commit}" != "${head_commit}" ]; then
    echo "error: the artifact in ${stage} was built from ${staged_commit}, agent HEAD is ${head_commit}" >&2
    echo "       drop --skip-agent-build to rebuild, or delete ${stage} and rerun" >&2
    exit 2
fi
if [ -f "${stage}/commit.txt" ] && [ "$(tr -d '\n' < "${stage}/commit.txt")" != "${head_commit}" ]; then
    echo "error: ${stage}/commit.txt does not match agent HEAD (was the stage dir mixed by hand?)" >&2
    exit 2
fi
echo "  staged commit ${staged_commit} == agent HEAD"

step "2/6 derive main.npdm (pure Python, original NPDM opened read-only)"
python3 "${agent_dir}/tools/make-minimal-npdm.py" \
    --input "${original_npdm}" --outdir "${stage}/npdm" | tail -5

step "3/6 release gate + import of this repo's assets"
python3 "${repo_dir}/tools/import-agent-release.py" \
    --nso "${stage}/acnh-agent.nso" \
    --npdm "${stage}/npdm/main.npdm" \
    --version-json "${stage}/version.json" \
    --original-npdm "${original_npdm}"

if [ "${skip_nro}" -eq 0 ]; then
    step "4/6 build the NRO (embeds data/)"
    "${repo_dir}/tools/build.sh"
else
    step "4/6 skipping the NRO build"
fi

step "5/6 verify: lock <-> release record <-> data/ <-> NRO"
if [ "${skip_nro}" -eq 0 ]; then
    python3 "${repo_dir}/tools/verify-release.py" --nro "${repo_dir}/acnh-manager.nro"
else
    python3 "${repo_dir}/tools/verify-release.py"
fi

if [ "${skip_store}" -eq 0 ]; then
    step "6/6 build the store package"
    python3 "${repo_dir}/tools/make-store-package.py"
else
    step "6/6 skipping the store package"
fi

echo
echo "release chain done: agent $(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["agentVersion"])' "${repo_dir}/packaging/agent-lock.json")"
