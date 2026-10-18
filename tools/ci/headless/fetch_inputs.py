"""Download hash-verified Ubuntu rootfs and CA inputs for isolated local chroot checks."""

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def fetch(url: str, destination: Path, digest: str, headers: dict | None = None) -> None:
    if not destination.exists():
        request = urllib.request.Request(url, headers=headers or {})
        temporary = destination.with_suffix(destination.suffix + ".partial")
        try:
            with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as stream:
                while chunk := response.read(1024 * 1024):
                    stream.write(chunk)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"SHA-256 mismatch: {destination.name}")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"Cached input SHA-256 mismatch: {destination.name}")
    print(f"Verified {destination.name}: sha256:{digest}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    inputs = json.loads(Path(__file__).with_name("inputs.json").read_text())
    # Anonymous public registry token stays in memory and is never logged.
    with urllib.request.urlopen("https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/ubuntu:pull", timeout=30) as response:
        token = json.load(response)["token"]
    headers = {"Authorization": "Bearer " + token,
               "Accept": "application/vnd.oci.image.manifest.v1+json"}
    registry = "https://registry-1.docker.io/v2/library/ubuntu/"
    digest = inputs["image"].split("@sha256:")[1]
    fetch(registry + "manifests/sha256:" + digest, args.output / "manifest.json", digest, headers)
    for key, filename in (("image_config_sha256", "config.json"), ("image_layer_sha256", "base-layer.tar.gz")):
        fetch(registry + "blobs/sha256:" + inputs[key], args.output / filename, inputs[key], headers)
    fetch(inputs["bootstrap_ca_url"], args.output / "ca-certificates.deb", inputs["bootstrap_ca_sha256"])


if __name__ == "__main__":
    main()
