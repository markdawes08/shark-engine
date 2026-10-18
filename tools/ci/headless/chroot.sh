#!/bin/bash
# Linux/WSL fallback when Docker is unavailable. No host packages are installed.
# First: python tools/ci/headless/fetch_inputs.py --output .tools/headless
# Then, as root: bash tools/ci/headless/chroot.sh /var/tmp/shark-headless-UNIQUE .tools/headless out/ci/headless-UNIQUE
# Rootfs and evidence paths must be fresh. Retain or remove the private rootfs
# only after reviewing evidence; this script never recursively deletes it.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
    test "$(id -u)" = 0 || { echo 'A disposable chroot requires root inside Linux/WSL' >&2; exit 2; }
    exec unshare --mount --fork bash "$0" --inside "$@"
fi
shift
test "$#" = 3 || { echo 'Expected ROOTFS_PARENT INPUT_DIRECTORY OUTPUT_DIRECTORY' >&2; exit 2; }
task_parent=$(realpath -m -- "$1")
test "$(dirname "$task_parent")" = /var/tmp || { echo 'Rootfs parent must be directly below /var/tmp' >&2; exit 2; }
case "$task_parent" in /var/tmp/shark-headless-*) ;; *) echo 'Use a new /var/tmp/shark-headless-* parent' >&2; exit 2;; esac
test ! -e "$task_parent"
cd "$(dirname "$0")/../../.."
task_source=$(pwd -P)
task_inputs=$(cd "$2" && pwd -P)
task_output=$3
mkdir -p "$task_output"
task_output=$(cd "$task_output" && pwd -P)
test -z "$(find "$task_output" -mindepth 1 -maxdepth 1 -print -quit)" || { echo 'Use a fresh output directory' >&2; exit 2; }
echo 'f2b4ef06ff01a67d4bed7657a4ae379a7d07934b7c3e743454a22a0d5275fb2b  '"$task_inputs/base-layer.tar.gz" | sha256sum --check -
echo '641de77d8f142cfd62a1a6f964ba67b20754d3337c480efb529d086075a06c9a  '"$task_inputs/ca-certificates.deb" | sha256sum --check -
mount --make-rprivate /
task_build=$task_parent/build-root
task_runtime=$task_parent/runtime-root
mkdir -p "$task_build" "$task_runtime"
for task_root in "$task_build" "$task_runtime"; do
    tar -xzf "$task_inputs/base-layer.tar.gz" -C "$task_root"
    mkdir -p "$task_root/proc" "$task_root/dev"
    mount -t proc proc "$task_root/proc"
    mount -t tmpfs tmpfs "$task_root/dev"
    for task_device in 'null 1 3' 'zero 1 5' 'random 1 8' 'urandom 1 9'; do
        read -r task_name task_major task_minor <<< "$task_device"
        mknod -m 666 "$task_root/dev/$task_name" c "$task_major" "$task_minor"
    done
    mkdir -p "$task_root/dev/pts"
    mount -t devpts devpts "$task_root/dev/pts"
    ln -s pts/ptmx "$task_root/dev/ptmx"
    ln -s /proc/self/fd "$task_root/dev/fd"
done
cp /etc/resolv.conf "$task_build/etc/resolv.conf"
mkdir -p "$task_build/opt/shark-headless" "$task_build/source" "$task_build/evidence"
cp tools/ci/headless/{inputs.json,setup.sh} requirements-dev.txt "$task_build/opt/shark-headless/"
cp "$task_inputs/ca-certificates.deb" "$task_build/tmp/shark-ca.deb"
timeout --kill-after=5 1500 chroot "$task_build" /bin/bash /opt/shark-headless/setup.sh 2>&1 | tee "$task_output/image-build.log"
mount --bind "$task_source" "$task_build/source"
mount -o remount,bind,ro "$task_build/source"
mount --bind "$task_output" "$task_build/evidence"
timeout --kill-after=5 1800 chroot "$task_build" /usr/bin/env -i \
    PATH=/opt/shark-headless/venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/root PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 \
    python /source/tools/headless_ci.py --build-root /work/build --output /evidence \
    2>&1 | tee "$task_output/headless.log"
mkdir -p "$task_runtime/package/bin" "$task_runtime/package/licenses/EnTT" "$task_runtime/package/licenses/GLM"
cp "$task_output/server-only/package/bin/Server" "$task_runtime/package/bin/Server"
cp "$task_output/server-only/package/licenses/EnTT/LICENSE" "$task_runtime/package/licenses/EnTT/LICENSE"
cp "$task_output/server-only/package/licenses/GLM/copying.txt" "$task_runtime/package/licenses/GLM/copying.txt"
chmod 755 "$task_runtime/package/bin/Server"
chroot "$task_runtime" dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' > "$task_output/clean-runtime-packages.tsv"
find "$task_runtime/package" -type f -printf '%P\n' > "$task_output/clean-runtime-files.txt"
timeout --kill-after=5 10 unshare --net chroot "$task_runtime" /usr/bin/env -i PATH=/usr/bin:/bin \
    /package/bin/Server --run-for-ms 20 2>&1 | tee "$task_output/clean-runtime.log"
printf '{"status":"PASS","scope":"installed package in pristine pinned Ubuntu userspace chroot, isolated network, shared host kernel; rootfs is writable"}\n' > "$task_output/clean-runtime.json"
uname -a > "$task_output/chroot-host-kernel.txt"
