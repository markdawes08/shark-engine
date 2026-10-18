#!/bin/bash
# Executed only in the pinned disposable image/chroot, never on the host distro.
set -euo pipefail
test "$(dpkg --print-architecture)" = amd64
grep -qx 'VERSION_ID="24.04"' /etc/os-release
task_home=/opt/shark-headless
mkdir -p "$task_home/provenance" /etc/ssl/certs
echo '641de77d8f142cfd62a1a6f964ba67b20754d3337c480efb529d086075a06c9a  /tmp/shark-ca.deb' | sha256sum --check -
# Bootstrap TLS trust from a hash-verified Ubuntu package. APT also verifies the
# signed snapshot metadata; no insecure repository or disabled TLS verification.
dpkg-deb -x /tmp/shark-ca.deb /
cat /usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt
find /etc/apt/sources.list.d -type f -delete
: > /etc/apt/sources.list
cat > /etc/apt/sources.list.d/ubuntu.sources <<'EOF'
Types: deb
URIs: https://snapshot.ubuntu.com/ubuntu/20261017T000000Z/
Suites: noble noble-updates noble-security
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
Check-Valid-Until: no
EOF
# Keep fetched archives until their digests have been recorded. This is an image
# input change, not a floating upgrade or a modification to the host APT config.
rm -f /etc/apt/apt.conf.d/docker-clean
export DEBIAN_FRONTEND=noninteractive
timeout --kill-after=5 300 apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=2 -o Acquire::https::Timeout=30 update
timeout --kill-after=5 900 apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=30 install -y --no-install-recommends \
    ca-certificates g++-13 clang-18 libclang-rt-18-dev python3 python3-venv git binutils
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' | LC_ALL=C sort > "$task_home/provenance/packages.tsv"
sha256sum /var/cache/apt/archives/*.deb > "$task_home/provenance/deb-sha256.txt"
cp /etc/apt/sources.list.d/ubuntu.sources "$task_home/provenance/ubuntu.sources"
cp /var/lib/apt/lists/*InRelease "$task_home/provenance/"
cp "$task_home/inputs.json" "$task_home/provenance/inputs.json"
cp /etc/os-release "$task_home/provenance/os-release"
find /usr/share/doc -name copyright -type f -print0 > "$task_home/provenance/license-paths.bin"
tar --null -T "$task_home/provenance/license-paths.bin" -czf "$task_home/provenance/system-licenses.tar.gz" /usr/share/common-licenses
python3 -m venv "$task_home/venv"
timeout --kill-after=5 300 "$task_home/venv/bin/python" -m pip install --disable-pip-version-check --require-hashes --only-binary=:all: \
    --report "$task_home/provenance/pip-install.json" -r "$task_home/requirements-dev.txt"
"$task_home/venv/bin/python" --version > "$task_home/provenance/python-version.txt"
