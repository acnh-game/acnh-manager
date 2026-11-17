#!/usr/bin/env bash
# ACNH-Manager release chain.  A full release builds the acnh-agent artifacts in a throwaway
# container; --manager-only reuses the locked agent release without consulting or rebuilding
# the agent tree.
#
#   1) build the acnh-agent release form in the pinned image (make switch SEMANTIC_HOOK=1)
#   2) derive main.npdm with acnh-agent's make-minimal-npdm.py from the original NPDM
#   3) release gate + import -> packaging/agent-lock.json, packaging/agent/<ver>/, data/
#   4) compile hbl from source and generate the forwarder NSP
#   5) build the NRO (data/ is embedded into .rodata)
#   6) publish the NRO into packaging/nro/ (that raw URL is what players and the store fetch)
#   7) dual-sign the served manifest (legacy .sig + .ed25519.sig)
#   8) verify: lock <-> release record <-> data/ <-> NRO <-> signature
#   9) build the store package
#
# Usage:
#   ./tools/release.sh                     # whole chain
#   ./tools/release.sh --skip-agent-build  # reuse the stage in build/scratch/agent-release
#   ./tools/release.sh --skip-nro --skip-store
#   ./tools/release.sh --allow-dirty       # development only: validate the chain itself
#   ./tools/release.sh --prepare-forwarder # regenerate data/forwarder_* for the next release
#   ./tools/release.sh --manager-only      # clean build: reuse locked agent + committed forwarder
#   ./tools/release.sh --key ~/.switch/acnh-manager-signing-key.pem \
#       --ed25519-key ~/.switch/acnh-signing-key.pem
#
# Needs docker (pinned image devkitpro/devkita64:20260219); inside the sandbox docker needs
# escalation.  Both signing keys live outside this repository: the legacy P-256 key is
# documented by tools/make-signing-key.py, and the new Ed25519 key by
# tools/make-ed25519-signing-key.py.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${ACNH_AGENT_DIR:-}" ]; then
    agent_dir="${ACNH_AGENT_DIR}"
elif [ -d "${repo_dir}/../acnh-agent" ]; then
    agent_dir="$(cd "${repo_dir}/../acnh-agent" && pwd)"
else
    agent_dir=""
fi
workspace_dir="$(cd "${repo_dir}/../.." && pwd)"
image="devkitpro/devkita64:20260219"
stage="${repo_dir}/build/scratch/agent-release"
# Original ACNH main.npdm -- the only input of the NPDM derivation; acnh-agent keeps it
# in the canonical workspace dump.
original_npdm="${workspace_dir}/dump/acnh-3.0.3/exefs/main.npdm"

skip_agent_build=0
skip_nro=0
skip_store=0
allow_dirty=0
manager_only=0
prepare_forwarder=0
# The two private halves of the keys compiled into the app.  Both stay outside this repository.
signing_key="${ACNH_MANAGER_SIGNING_KEY:-${HOME}/.switch/acnh-manager-signing-key.pem}"
ed25519_signing_key="${ACNH_SIGNING_KEY:-${HOME}/.switch/acnh-signing-key.pem}"
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-agent-build) skip_agent_build=1 ;;
        --skip-nro)         skip_nro=1 ;;
        --skip-store)       skip_store=1 ;;
        --allow-dirty)      allow_dirty=1 ;;
        --manager-only)     manager_only=1; skip_agent_build=1 ;;
        --prepare-forwarder) prepare_forwarder=1 ;;
        --key)              signing_key="$2"; shift ;;
        --ed25519-key)      ed25519_signing_key="$2"; shift ;;
        -h|--help)          sed -n "2,29p" "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { echo; echo "=== $* ==="; }

if [ "${prepare_forwarder}" -eq 1 ]; then
    step "prepare forwarder"
    python3 "${repo_dir}/tools/build-forwarder.py"
    echo "forwarder is ready; commit data/forwarder_nsp.bin and data/forwarder_nsp_meta.bin"
    echo "before running tools/release.sh --manager-only from a clean tree"
    exit 0
fi

step "0/9 preflight"
if [ "${manager_only}" -eq 0 ]; then
    [ -d "${agent_dir}" ] || { echo "error: acnh-agent not found at ${agent_dir} (override with ACNH_AGENT_DIR)" >&2; exit 2; }
fi
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
if [ "${manager_only}" -eq 0 ]; then
    head_commit="$(git -C "${agent_dir}" rev-parse --short=12 HEAD)"
    if [ -n "$(git -C "${agent_dir}" status --porcelain)" ]; then
        echo "error: acnh-agent working tree is dirty; a release must come from a clean commit (otherwise version.json carries dirty=true and the gate refuses it)" >&2
        exit 2
    fi
    [ -f "${original_npdm}" ] || { echo "error: original main.npdm not found: ${original_npdm}" >&2; exit 2; }
else
    [ -f "${repo_dir}/packaging/agent-lock.json" ] || {
        echo "error: --manager-only needs the existing release lock" >&2
        exit 2
    }
    reused_agent_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["agentVersion"])' "${repo_dir}/packaging/agent-lock.json")"
    reused_agent_commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "${repo_dir}/packaging/agent-lock.json")"
    [ -d "${repo_dir}/packaging/agent/${reused_agent_version}" ] || {
        echo "error: --manager-only needs packaging/agent/${reused_agent_version}" >&2
        exit 2
    }
fi
# The app version lives in exactly one place (the Makefile); the published NRO's file name and
# the store's update asset both take it from here, so they cannot drift apart.
app_version="$(sed -n 's/^APP_VERSION[[:space:]]*:=[[:space:]]*//p' "${repo_dir}/Makefile" | head -1 | tr -d '[:space:]')"
[ -n "${app_version}" ] || { echo "error: APP_VERSION not found in ${repo_dir}/Makefile" >&2; exit 2; }
# A release without a signature is a release nobody can install from: every shipped app
# verifies the manifest before trusting it, so the key has to be here.
[ -f "${signing_key}" ] || {
    echo "error: release signing key not found: ${signing_key}" >&2
    echo "       pass --key <path> (or set ACNH_MANAGER_SIGNING_KEY);" >&2
    echo "       generate one once with tools/make-signing-key.py" >&2
    exit 2
}
[ -f "${ed25519_signing_key}" ] || {
    echo "error: Ed25519 signing key not found: ${ed25519_signing_key}" >&2
    echo "       pass --ed25519-key <path> (or set ACNH_SIGNING_KEY);" >&2
    echo "       generate it once with tools/make-ed25519-signing-key.py" >&2
    exit 2
}
if [ "${manager_only}" -eq 0 ]; then
    echo "acnh-agent: ${agent_dir} @ ${head_commit} (clean tree)"
else
    echo "acnh-agent: ${reused_agent_version} @ ${reused_agent_commit} (locked release reused; not rebuilt)"
fi
echo "app version: ${app_version}"
echo "P-256 signing key : ${signing_key}"
echo "Ed25519 signing key: ${ed25519_signing_key}"

if [ "${manager_only}" -eq 1 ]; then
    step "1/9-3/9 manager-only: reuse the locked agent release and embedded payload"
elif [ "${skip_agent_build}" -eq 0 ]; then
    step "1/9 build the acnh-agent release form in the container (SEMANTIC_HOOK=1)"
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
    step "1/9 skipping the agent build (reusing ${stage})"
    [ -f "${stage}/acnh-agent.nso" ] || { echo "error: ${stage} has no artifact from a previous build" >&2; exit 2; }
fi

if [ "${manager_only}" -eq 0 ]; then
    step "1b/9 the staged artifact must come from the current agent HEAD"
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

    step "2/9 derive main.npdm (pure Python, original NPDM opened read-only)"
    python3 "${agent_dir}/tools/make-minimal-npdm.py" \
        --input "${original_npdm}" --outdir "${stage}/npdm" | tail -5

    step "3/9 release gate + import of this repo's assets"
    python3 "${repo_dir}/tools/import-agent-release.py" \
        --nso "${stage}/acnh-agent.nso" \
        --npdm "${stage}/npdm/main.npdm" \
        --version-json "${stage}/version.json" \
        --original-npdm "${original_npdm}"
fi

if [ "${skip_nro}" -eq 0 ]; then
    if [ "${manager_only}" -eq 0 ]; then
        step "4/9 compile hbl from source and generate the forwarder NSP"
        python3 "${repo_dir}/tools/build-forwarder.py"
    else
        step "4/9 manager-only: verify the committed forwarder version"
        jq -e --arg version "${app_version}" \
            '.displayVersion == $version' \
            "${repo_dir}/data/forwarder_nsp_meta.bin" >/dev/null || {
            echo "error: forwarder displayVersion does not match APP_VERSION ${app_version}" >&2
            echo "       run tools/release.sh --prepare-forwarder, commit data/forwarder_*," >&2
            echo "       then rerun --manager-only from a clean tree" >&2
            exit 2
        }
    fi
    step "5/9 build the NRO (embeds data/)"
    # --release: development aids (probes, text UI) are left out of this build.
    "${repo_dir}/tools/build.sh" --release
    # And prove it: the shipped binary must not contain the probes' own strings.
    if grep -aq "large-file probe\|wprobe\[" "${repo_dir}/acnh-manager.nro"; then
        echo "release check failed: the NRO still carries development probes" >&2
        exit 1
    fi
    echo "release check: no development probes in acnh-manager.nro"
else
    step "4/9-5/9 skipping the forwarder and NRO build"
fi

step "6/9 publish the NRO into packaging/nro/ (served by Gitee raw; the store links here)"
if [ "${skip_nro}" -eq 0 ]; then
    mkdir -p "${repo_dir}/packaging/nro"
    cp "${repo_dir}/acnh-manager.nro" "${repo_dir}/packaging/nro/acnh-manager-${app_version}.nro"
    echo "  packaging/nro/acnh-manager-${app_version}.nro <- acnh-manager.nro"
    echo "  commit it with the release: that raw URL is what players and the store download"
    python3 "${repo_dir}/tools/update-manager-manifest.py" \
        --version "${app_version}" \
        --nro "${repo_dir}/packaging/nro/acnh-manager-${app_version}.nro"
else
    echo "  skipped with --skip-nro"
fi

step "7/9 dual-sign the served manifest (legacy P-256 + Ed25519)"
python3 "${repo_dir}/tools/sign-manifest.py" \
    --key "${signing_key}" \
    --ed25519-key "${ed25519_signing_key}"

step "8/9 verify: lock <-> release record <-> data/ <-> NRO <-> signature"
if [ "${skip_nro}" -eq 0 ]; then
    python3 "${repo_dir}/tools/verify-release.py" --nro "${repo_dir}/acnh-manager.nro"
else
    python3 "${repo_dir}/tools/verify-release.py"
fi

if [ "${skip_store}" -eq 0 ]; then
    step "9/9 build the store package"
    python3 "${repo_dir}/tools/make-store-package.py"
else
    step "9/9 skipping the store package"
fi

echo
echo "release chain done: agent $(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["agentVersion"])' "${repo_dir}/packaging/agent-lock.json")"
