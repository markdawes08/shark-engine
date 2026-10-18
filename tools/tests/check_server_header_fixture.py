"""Build real hidden-header leaks, then reject their compiler-discovered inputs."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_server_closure import ClosureError, compiler_headers, ninja_dependencies, system_header_owners
from process_runner import run_process


def run(command, *, cwd=None):
    result = run_process(command, cwd=cwd, timeout=20)
    if result.returncode or result.timed_out or result.cleanup_errors:
        raise RuntimeError(f"Fixture command failed: {command}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.source_dir.resolve()
    for mode in range(3):
        build = (args.build_dir / str(mode)).resolve()
        run([args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
             "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DCMAKE_MAKE_PROGRAM=" + args.ninja,
             "-DSHARK_HEADER_LEAK=" + str(mode)])
        run([args.cmake, "--build", str(build), "--parallel", "2"])
        output = run([args.ninja, "-t", "deps"], cwd=build)
        groups = ninja_dependencies(output)
        try:
            _, external = compiler_headers(output, [{"object": item} for item in groups if item.endswith((".o", ".obj"))],
                                          source, build, linker_outputs={"Server", "Server.exe"})
            if external:
                # GCC injects libc's stdc-predef.h even into a header-only fixture.
                # Apply the real closure's package ownership policy; do not drop
                # implicit inputs or assume every native compiler emits none.
                system_header_owners(run(["dpkg-query", "-S", *external]), external)
        except ClosureError as error:
            if mode == 0 or "Non-Core source" not in str(error):
                raise
        else:
            if mode != 0:
                raise RuntimeError(f"Compiler-discovered client header leak {mode} was accepted")
    print("PASS: real Ninja dependency records accept Core headers and reject relative/generated client header leaks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
