# The architecture-specific manifest and all APT inputs are locked in inputs.json.
FROM ubuntu@sha256:a2d43bffc46670d7108bb3c0468c67c5557c4c92e83e1e34cbff1322ad1b94dc
ADD --checksum=sha256:641de77d8f142cfd62a1a6f964ba67b20754d3337c480efb529d086075a06c9a https://snapshot.ubuntu.com/ubuntu/20261017T000000Z/pool/main/c/ca-certificates/ca-certificates_20240203_all.deb /tmp/shark-ca.deb
COPY inputs.json setup.sh requirements-dev.txt /opt/shark-headless/
RUN /bin/bash /opt/shark-headless/setup.sh
ENV PATH="/opt/shark-headless/venv/bin:${PATH}" \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1
WORKDIR /source
CMD ["python", "tools/headless_ci.py", "--build-root", "/work/build", "--output", "/evidence"]
