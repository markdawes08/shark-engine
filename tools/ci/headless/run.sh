#!/bin/bash
# Local Linux Docker entry point; CI uses this same bounded path.
# Usage: bash tools/ci/headless/run.sh [out/ci/headless] [--inject-test-failure]
set -euo pipefail
cd "$(dirname "$0")/../../.."
task_source=$(pwd -P)
task_output=${1:-out/ci/headless}
shift "$(( $# > 0 ? 1 : 0 ))"
mkdir -p "$task_output"
task_output=$(cd "$task_output" && pwd -P)
test -z "$(find "$task_output" -mindepth 1 -maxdepth 1 -print -quit)" || { echo 'Use a fresh output directory' >&2; exit 2; }
task_context=$(mktemp -d)
task_container="shark-headless-${GITHUB_RUN_ID:-local}-$$"
task_runtime_container="${task_container}-runtime"
trap 'timeout --kill-after=5 15 docker rm -f "$task_container" "$task_runtime_container" >/dev/null 2>&1 || true; rm -rf "$task_context"' EXIT
cp tools/ci/headless/{Dockerfile,inputs.json,setup.sh} requirements-dev.txt "$task_context/"
timeout --kill-after=5 15 docker version --format '{{json .}}' > "$task_output/docker-version.json"
timeout --kill-after=5 1500 docker build --platform linux/amd64 --progress plain --tag "$task_container" "$task_context" 2>&1 | tee "$task_output/image-build.log"
timeout --kill-after=5 15 docker image inspect "$task_container" > "$task_output/image-inspect.json"
timeout --kill-after=5 1800 docker run --name "$task_container" --platform linux/amd64 \
    --mount "type=bind,src=$task_source,dst=/source,readonly" \
    --mount "type=bind,src=$task_output,dst=/evidence" \
    --env GITHUB_SHA --env GITHUB_RUN_ID --env GITHUB_RUN_ATTEMPT \
    "$task_container" python tools/headless_ci.py --build-root /work/build --output /evidence "$@" \
    2>&1 | tee "$task_output/headless.log"
# The release package also starts in the unmodified base image, without build
# tools, Python, display/audio devices, networking or a writable root filesystem.
task_base=ubuntu@sha256:a2d43bffc46670d7108bb3c0468c67c5557c4c92e83e1e34cbff1322ad1b94dc
timeout --kill-after=5 30 docker run --rm --name "$task_runtime_container" --platform linux/amd64 --network none --read-only \
    "$task_base" dpkg-query -W '-f=${binary:Package}\t${Version}\t${Architecture}\n' \
    > "$task_output/clean-runtime-packages.tsv"
timeout --kill-after=5 30 docker run --rm --name "$task_runtime_container" --platform linux/amd64 --network none --read-only \
    --mount "type=bind,src=$task_output/server-only/package,dst=/package,readonly" \
    "$task_base" /package/bin/Server --run-for-ms 20 \
    2>&1 | tee "$task_output/clean-runtime.log"
printf '{"status":"PASS","scope":"installed package in immutable base, network disabled, read-only root"}\n' > "$task_output/clean-runtime.json"
