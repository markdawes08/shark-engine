"""Adversarial M01 closure evidence: labels alone cannot establish isolation."""

from pathlib import Path
import json
import hashlib
import io
import tarfile
import zipfile
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_server_closure import (ClosureError, codemodel_source, compile_inventory, compiler_headers, dynamic_needed,
                                  file_api, link_inventory, ninja_dependencies, package_inventory,
                                  resolved_libraries, source_path, system_header_owners, TLS_STATIC, TLS_OBJECT, TLS_UTILITIES)
from core_dependencies import MBEDTLS_LICENSE_FILES, DEPENDENCIES
from storage_dependency import DEFINITIONS as SQLITE_DEFINITIONS


class ServerClosureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shark-closure-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.package = self.root / "package"
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        (self.source / "engine/include").mkdir(parents=True)
        for relative in ("engine/core/src/core.cpp", "server/main.cpp", "server/storage/src/store.cpp"):
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int fixture;\n", encoding="utf-8")
        self.targets = [
            {"name": "EngineCore", "id": "core", "type": "STATIC_LIBRARY", "artifacts": [{"path": "engine/core/libEngineCore.a"}], "sources": [
                {"path": "engine/core/src/core.cpp", "compileGroupIndex": 0}],
             "compileGroups": [{"language": "CXX", "includes": [{"path": str(self.source / "engine/include")}]}]},
            {"name": "Server", "id": "server", "type": "EXECUTABLE", "link": {"commandFragments": [
                {"role": "flags", "fragment": "-O3 -DNDEBUG"}, {"role": "libraries", "fragment": "engine/core/libEngineCore.a"}]}, "sources": [
                {"path": "server/main.cpp", "compileGroupIndex": 0}], "dependencies": [{"id": "core"}, {"id": "guard"}]},
            {"name": "shark_check_server_boundary", "id": "guard", "type": "UTILITY"},
        ]
        for name in sorted(TLS_STATIC | TLS_OBJECT | TLS_UTILITIES):
            target = {"name": name, "id": name, "type": "UTILITY"}
            if name in TLS_STATIC | TLS_OBJECT:
                relative = "_deps/mbedtls-src/library/" + name + ".c"
                artifact = "_deps/mbedtls-build/" + name + (".a" if name in TLS_STATIC else ".o")
                target.update(type="STATIC_LIBRARY" if name in TLS_STATIC else "OBJECT_LIBRARY",
                              artifacts=[{"path": artifact}], sources=[{"path": str(self.build / relative), "compileGroupIndex": 0}],
                              compileGroups=[{"language": "C"}])
                path = self.build / artifact
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"built TLS fixture")
            self.targets.append(target)
        self.targets[1]["link"]["commandFragments"][1]["fragment"] += " " + " ".join(
            "_deps/mbedtls-build/" + name + ".a" for name in ("SharkTLS_mbedtls", "SharkTLS_mbedx509", "SharkTLS_tfpsacrypto"))
        self.targets[1]["link"]["commandFragments"][1]["fragment"] = (
            "server/libEngineStorage.a " + self.targets[1]["link"]["commandFragments"][1]["fragment"] + " libSharkSQLite.a")
        self.targets += [
            {"name": "EngineStorage", "id": "storage", "type": "STATIC_LIBRARY",
             "artifacts": [{"path": "server/libEngineStorage.a"}],
             "sources": [{"path": "server/storage/src/store.cpp", "compileGroupIndex": 0}],
             "compileGroups": [{"language": "CXX"}], "dependencies": [{"id": "core"}, {"id": "sqlite"}]},
            {"name": "SharkSQLite", "id": "sqlite", "type": "STATIC_LIBRARY",
             "artifacts": [{"path": "libSharkSQLite.a"}],
             "sources": [{"path": str(self.build / "_deps/sqlite-src/sqlite3.c"), "compileGroupIndex": 0}],
             "compileGroups": [{"language": "C"}]}]
        self.targets[1]["dependencies"].append({"id": "storage"})
        self.configuration = {"name": "Release", "directories": [{"source": value} for value in (".", "engine", "engine/core", "server")],
                              "targets": [{"name": item["name"], "id": item["id"], "jsonFile": item["id"] + ".json"}
                                          for item in self.targets]}
        self.model = {"paths": {"source": str(self.source), "build": str(self.build)}, "configurations": [self.configuration]}
        self.cache = {"entries": [{"name": name, "value": value} for name, value in {
            "SHARK_BUILD_CLIENT": "OFF", "SHARK_BUILD_EDITOR": "OFF", "SHARK_BUILD_TESTS": "OFF",
            "SHARK_BUILD_GRAPHICS_PROBE": "OFF",
            "SHARK_BUILD_SERVER": "ON", "SHARK_SANITIZER": "none", "CMAKE_GENERATOR": "Ninja",
            "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"}.items()]}
        self.commands = [{"directory": str(self.build), "file": str(self.source / relative),
                          "arguments": ["c++", "-I" + str(self.source / "engine/include"), "-c", str(self.source / relative), "-o", relative + ".o"]}
                         for relative in ("engine/core/src/core.cpp", "server/main.cpp", "server/storage/src/store.cpp")]
        for relative in ("engine/core/libEngineCore.a", "engine/core/src/core.cpp.o", "server/main.cpp.o",
                         "server/storage/src/store.cpp.o", "server/libEngineStorage.a", "libSharkSQLite.a", "sqlite3.c.o"):
            path = self.build / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"built fixture")
        self.add_core_dependency_fixtures()
        self.add_storage_dependency_fixture()
        self.commands.append({"directory": str(self.build), "file": str(self.build / "_deps/sqlite-src/sqlite3.c"),
                              "arguments": ["cc", *["-D" + item for item in SQLITE_DEFINITIONS], "-c",
                                            str(self.build / "_deps/sqlite-src/sqlite3.c"), "-o", "sqlite3.c.o"]})
        for target in self.targets:
            if target["name"] in TLS_STATIC | TLS_OBJECT:
                path = target["sources"][0]["path"]
                artifact = target["artifacts"][0]["path"]
                object_path = artifact if target["name"] in TLS_OBJECT else artifact + ".o"
                output = self.build / object_path
                output.write_bytes(b"built TLS object")
                self.commands.append({"directory": str(self.build), "file": path,
                                      "arguments": ["cc", "-c", path, "-o", object_path]})
        self.write_evidence()
        (self.package / "licenses/EnTT").mkdir(parents=True)
        (self.package / "licenses/EnTT/LICENSE").write_bytes(b"fixture license")
        (self.package / "licenses/GLM").mkdir()
        (self.package / "licenses/GLM/copying.txt").write_bytes(b"fixture license")
        (self.package / "licenses/SQLite").mkdir()
        (self.package / "licenses/SQLite/LICENSE.txt").write_bytes(b"/* fixture notice */\n")
        for relative in MBEDTLS_LICENSE_FILES:
            path = self.package / "licenses/MbedTLS" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture license")
        (self.package / "bin").mkdir(parents=True)
        binary = self.package / "bin/Server"
        binary.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00" + bytes(64))
        binary.chmod(0o755)

    def add_core_dependency_fixtures(self):
        dependencies = {}
        for name, license_file, header in (("EnTT", "LICENSE", "src/entt/entity/registry.hpp"),
                                          ("GLM", "copying.txt", "glm/glm.hpp"),
                                          ("MbedTLS", "LICENSE", "include/mbedtls/ssl.h")):
            lower = name.lower()
            contents = {license_file: b"fixture license", header: b"// fixture header"}
            if name == "GLM":
                contents["glm/detail/fixture.inl"] = b"// fixture inline header"
            if name == "MbedTLS":
                contents.update({item: b"fixture license" for item in MBEDTLS_LICENSE_FILES})
                contents.update({"library/" + item + ".c": b"int tls_fixture;" for item in TLS_STATIC | TLS_OBJECT})
            archive = self.root / (lower + ".tar.gz")
            with tarfile.open(archive, "w:gz") as stream:
                for path, data in contents.items():
                    item = tarfile.TarInfo(lower + "-" + "a" * 40 + "/" + path)
                    item.size = len(data)
                    stream.addfile(item, io.BytesIO(data))
                if name == "MbedTLS":
                    for index in range(147):
                        item = tarfile.TarInfo(lower + "-" + "a" * 40 + "/tf-psa-crypto/drivers/pqcp/mldsa-native/test/link-" + str(index))
                        item.type = tarfile.SYMTYPE
                        item.linkname = "../LICENSE"
                        stream.addfile(item)
            dependencies[name] = {"version": "fixture", "commit": "a" * 40, "license": "Apache-2.0" if name == "MbedTLS" else "MIT",
                                  "license_files": list(MBEDTLS_LICENSE_FILES) if name == "MbedTLS" else [license_file],
                                  "archive_prefix": lower + "-" + "a" * 40, "patches": [], "transitive_dependencies": [],
                                  "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
            for suffix in ("-src", "-build", "-subbuild"):
                if name == "MbedTLS" and suffix == "-subbuild":
                    continue
                (self.build / "_deps" / (lower + suffix)).mkdir(parents=True, exist_ok=True)
            for path, data in contents.items():
                target = self.build / "_deps" / (lower + "-src") / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            key = DEPENDENCIES[name]["cache"]
            self.cache["entries"].append({"name": key, "value": str(archive)})
        (self.source / "dependencies.lock.json").write_text(json.dumps({"dependencies": dependencies}))

    def add_storage_dependency_fixture(self):
        notice = b"/* fixture notice */\n"
        header = notice + b'#define SQLITE_VERSION        "fixture"\n#define SQLITE_SOURCE_ID      "fixture-id"\n'
        contents = {"sqlite3.h": header, "sqlite3.c": b"int sqlite_fixture;", "sqlite3ext.h": b"/* extension */", "shell.c": b"int shell_fixture;"}
        archive = self.root / "sqlite.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            for name, data in contents.items():
                stream.writestr("sqlite-fixture/" + name, data)
        locked = {"version": "fixture", "source_id": "fixture-id", "archive_prefix": "sqlite-fixture",
                  "archive_bytes": archive.stat().st_size, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                  "license": "LicenseRef-SQLite-Public-Domain", "license_files": ["LICENSE.txt"],
                  "patches": [], "transitive_dependencies": [], "compile_definitions": list(SQLITE_DEFINITIONS),
                  "source_sha256": {name: hashlib.sha256(contents[name]).hexdigest() for name in ("sqlite3.c", "sqlite3.h")},
                  "notice_sha256": hashlib.sha256(notice).hexdigest(), "notice_bytes": len(notice)}
        lock = self.source / "dependencies.lock.json"
        data = json.loads(lock.read_text())
        data["dependencies"]["SQLite"] = locked
        lock.write_text(json.dumps(data))
        directory = self.build / "_deps/sqlite-src"
        directory.mkdir()
        for name, content in {**contents, "LICENSE.txt": notice}.items():
            (directory / name).write_bytes(content)
        self.cache["entries"].append({"name": "SHARK_SQLITE_ARCHIVE_INPUT", "value": str(archive)})

    def write_evidence(self):
        data = {"index-1.json": {"objects": [
            {"kind": "codemodel", "version": {"major": 2}, "jsonFile": "model.json"},
            {"kind": "cache", "version": {"major": 2}, "jsonFile": "cache.json"}]},
            "model.json": self.model, "cache.json": self.cache}
        data.update({item["id"] + ".json": item for item in self.targets})
        for name, value in data.items():
            (self.reply / name).write_text(json.dumps(value), encoding="utf-8")
        (self.build / "compile_commands.json").write_text(json.dumps(self.commands), encoding="utf-8")

    def test_accepts_exact_core_server_graph_and_compile_inventory(self):
        source, _, targets, _ = file_api(self.build)
        self.assertEqual(len(compile_inventory(self.build, source, targets)), 14)

    def add_reviewed_crypto_fixture(self):
        cpp = self.source / "engine/core/src/crypto_runtime.cpp"
        header = cpp.with_suffix(".hpp")
        cpp.write_text("int shared_crypto_fixture;\n", encoding="utf-8")
        header.write_text("// Private Core crypto state.\n", encoding="utf-8")
        output = self.build / "engine/core/CMakeFiles/EngineCore.dir/src/crypto_runtime.cpp.o"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"reviewed Core object")
        self.targets[0]["sources"].append({"path": cpp.relative_to(self.source).as_posix(), "compileGroupIndex": 0})
        entry = {"directory": str(self.build), "file": str(cpp),
                 "arguments": ["c++", "-c", str(cpp), "-o", str(output)]}
        self.commands.append(entry)
        self.write_evidence()
        return cpp, header, output, entry

    def test_accepts_exact_private_core_crypto_source_object_and_header(self):
        cpp, header, output, _ = self.add_reviewed_crypto_fixture()
        source, _, targets, _ = file_api(self.build)
        compiled = compile_inventory(self.build, source, targets)
        self.assertEqual(len(compiled), 15)
        relative = output.relative_to(self.build).as_posix()
        evidence = f"{relative}: #deps 2, deps mtime 1 (VALID)\n    {cpp}\n    {header}\n"
        rows, system = compiler_headers(evidence, [{"path": str(cpp), "object": relative}], self.source, self.build)
        self.assertEqual({row["path"] for row in rows}, {str(cpp), str(header)})
        self.assertEqual(system, [])

    def test_crypto_review_rejects_lookalikes_and_generated_or_misplaced_inputs(self):
        for relative in ("engine/core/src/not_crypto_runtime.cpp", "engine/core/src/crypto_runtime.cpp.extra",
                         "engine/core/runtime/crypto_runtime.cpp", "runtime/crypto_runtime.cpp",
                         "engine/core/src/Crypto_Runtime.cpp"):
            with self.subTest(relative=relative):
                path = self.source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// Unreviewed input.\n", encoding="utf-8")
                with self.assertRaises(ClosureError):
                    source_path(path, self.source, self.build)
        generated = self.build / "engine/core/src/crypto_runtime.cpp"
        generated.parent.mkdir(parents=True, exist_ok=True)
        generated.write_text("// Generated lookalike.\n", encoding="utf-8")
        with self.assertRaises(ClosureError):
            source_path(generated, self.source, self.build, generated=True)

    def test_crypto_review_preserves_flag_and_compiler_identity_rejection(self):
        cpp, _, output, entry = self.add_reviewed_crypto_fixture()
        source, _, targets, _ = file_api(self.build)
        original = list(entry["arguments"])
        bad_input = self.source / "server/main.cpp"
        bad_output = self.build / "engine/core/crypto_runtime.cpp.o"
        bad_output.write_bytes(b"wrong location")
        variants = [original + ["-Dleak_runtime_enabled=1"], original + ["-I" + str(self.source / "runtime")],
                    ["c++", "-c", str(bad_input), "-o", str(output)],
                    ["c++", "-c", str(cpp), "-o", str(bad_output)]]
        for arguments in variants:
            with self.subTest(arguments=arguments):
                entry["arguments"] = arguments
                self.write_evidence()
                with self.assertRaises(ClosureError):
                    compile_inventory(self.build, source, targets)

    def test_rejects_client_target_even_if_unlinked(self):
        self.configuration["targets"].append({"name": "EngineClient", "id": "client", "jsonFile": "client.json"})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "target closure"):
            file_api(self.build)

    def test_rejects_enabled_graphics_probe_even_without_link(self):
        for item in self.cache["entries"]:
            if item["name"] == "SHARK_BUILD_GRAPHICS_PROBE":
                item["value"] = "ON"
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "SHARK_BUILD_GRAPHICS_PROBE=OFF"):
            file_api(self.build)

    def test_rejects_unlinked_probe_build_subtree(self):
        (self.build / "probes/graphics").mkdir(parents=True)
        with self.assertRaisesRegex(ClosureError, "forbidden build subtree: probes"):
            file_api(self.build)

    def test_rejects_hidden_generated_build_dependency(self):
        self.targets[0]["dependencies"] = [{"id": "authoring-generator"}]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "External build dependency"):
            file_api(self.build)

    def test_accepts_core_link_and_rejects_static_link_option_bypasses(self):
        self.assertEqual(len(link_inventory(self.targets, self.build)), 2)
        for entry in ({"role": "flags", "fragment": "/tmp/libclient.a"},
                      {"role": "flags", "fragment": "-Wl,@hidden.rsp"},
                      {"role": "libraryPath", "fragment": "-L/tmp/hidden"},
                      {"role": "libraries", "fragment": "/tmp/libclient.a"}):
            with self.subTest(entry=entry):
                self.targets[1]["link"]["commandFragments"].append(entry)
                with self.assertRaises(ClosureError):
                    link_inventory(self.targets, self.build)
                self.targets[1]["link"]["commandFragments"].pop()

    def test_rejects_relative_and_generated_client_header_leaks(self):
        leak = self.source / "engine/client/leak.hpp"
        leak.parent.mkdir(parents=True)
        leak.write_text("inline int client_value(){return 7;}", encoding="utf-8")
        generated = self.build / "engine/core/generated.hpp"
        generated.write_text('#include "' + leak.as_posix() + '"', encoding="utf-8")
        for paths in ([str(leak)], [str(generated), str(leak)]):
            evidence = "engine/core/core.o: #deps " + str(len(paths)) + ", deps mtime 1 (VALID)\n"
            evidence += "".join("    " + path + "\n" for path in paths)
            with self.subTest(paths=paths), self.assertRaisesRegex(ClosureError, "Non-Core source"):
                compiler_headers(evidence, [{"object": "engine/core/core.o"}], self.source, self.build)

    def test_rejects_stale_missing_and_truncated_ninja_header_evidence(self):
        for evidence in ("", "core.o: #deps 1, deps mtime 1 (STALE)\n    /core.hpp\n",
                         "core.o: #deps 2, deps mtime 1 (VALID)\n    /core.hpp\n"):
            with self.subTest(evidence=evidence), self.assertRaises(ClosureError):
                ninja_dependencies(evidence)
        with self.assertRaisesRegex(ClosureError, "objects differ"):
            compiler_headers("extra.o: #deps 1, deps mtime 1 (VALID)\n    /core.hpp\n",
                             [{"object": "core.o"}], self.source, self.build)

    def test_ninja_linker_record_is_separate_from_compiler_header_records(self):
        source = self.source / "engine/core/src/core.cpp"
        output = f"core.o: #deps 1, deps mtime 1 (VALID)\n    {source}\n\n"
        output += "bin/Server: #deps 2, deps mtime 1 (VALID)\n    core.o\n    /usr/lib/libc.so\n"
        headers, system = compiler_headers(output, [{"object": "core.o"}], self.source, self.build,
                                          linker_outputs={"bin/Server"})
        self.assertEqual([entry["path"] for entry in headers], [str(source)])
        self.assertEqual(system, [])
        for unexpected in ("bin/Editor", "hidden.o"):
            with self.subTest(record=unexpected), self.assertRaisesRegex(ClosureError, "objects differ"):
                compiler_headers(output.replace("bin/Server:", unexpected + ":"), [{"object": "core.o"}],
                                 self.source, self.build, linker_outputs={"bin/Server"})

    def test_system_headers_require_standard_library_package_ownership(self):
        valid = "libc6-dev:amd64: /usr/include/features.h\nlibc6-dev:amd64: /usr/include/stdc-predef.h\n"
        self.assertEqual(system_header_owners(valid, ["/usr/include/features.h", "/usr/include/stdc-predef.h"]),
                         {"/usr/include/features.h": "libc6-dev", "/usr/include/stdc-predef.h": "libc6-dev"})
        with self.assertRaisesRegex(ClosureError, "Unreviewed system header package"):
            system_header_owners("libvulkan-dev:amd64: /usr/include/vulkan.h\n", ["/usr/include/vulkan.h"])
        with self.assertRaisesRegex(ClosureError, "Incomplete"):
            system_header_owners(valid, ["/usr/include/features.h", "/usr/include/unknown.h"])

    def test_rejects_configured_client_directory_without_target(self):
        self.configuration["directories"].append({"source": "engine/client"})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "configured directory"):
            file_api(self.build)

    def test_rejects_discovered_vulkan_cache_entry(self):
        self.cache["entries"].append({"name": "Vulkan_INCLUDE_DIR", "value": "/usr/include"})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "discovered dependency"):
            file_api(self.build)

    def test_rejects_tests_enabled_cache(self):
        next(item for item in self.cache["entries"] if item["name"] == "SHARK_BUILD_TESTS")["value"] = "ON"
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "SHARK_BUILD_TESTS=OFF"):
            file_api(self.build)

    def test_rejects_additional_dependency_download_tree(self):
        (self.build / "_deps/glfw-src").mkdir()
        with self.assertRaisesRegex(ClosureError, "exactly the locked Core"):
            file_api(self.build)

    def test_accepts_only_verified_entt_header_inputs(self):
        header = self.build / "_deps/entt-src/src/entt/entity/registry.hpp"
        self.targets[0]["compileGroups"][0]["includes"].append({"path": str(header.parents[2]), "isSystem": True})
        self.write_evidence()
        file_api(self.build)
        evidence = f"core.o: #deps 1, deps mtime 1 (VALID)\n    {header}\n"
        headers, system = compiler_headers(evidence, [{"object": "core.o"}], self.source, self.build)
        self.assertEqual([item["path"] for item in headers], [str(header)])
        self.assertEqual(system, [])
        header.write_bytes(b"tampered cache header")
        with self.assertRaisesRegex(ClosureError, "differs from the locked archive"):
            file_api(self.build)

    def test_tls_c_source_drift_is_rejected(self):
        path = self.build / "_deps/mbedtls-src/library/SharkTLS_mbedtls.c"
        path.write_bytes(b"unreviewed TLS code")
        with self.assertRaisesRegex(ClosureError, "Populated MbedTLS source differs"):
            file_api(self.build)

    def test_sqlite_source_header_notice_and_missing_archive_reject_drift(self):
        for name in ("sqlite3.c", "sqlite3.h", "LICENSE.txt"):
            path = self.build / "_deps/sqlite-src" / name
            original = path.read_bytes()
            with self.subTest(name=name):
                path.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ClosureError, "Populated SQLite source differs"):
                    file_api(self.build)
                path.write_bytes(original)
        self.cache["entries"] = [item for item in self.cache["entries"] if item["name"] != "SHARK_SQLITE_ARCHIVE_INPUT"]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Missing locked SQLite archive"):
            file_api(self.build)

    def test_sqlite_notice_required_and_database_or_tools_payload_rejected(self):
        notice = self.package / "licenses/SQLite/LICENSE.txt"
        notice.unlink()
        with self.assertRaisesRegex(ClosureError, "Incomplete Server package"):
            package_inventory(self.package)
        notice.write_bytes(b"fixture notice")
        for name in ("bin/sqlite3", "bin/libsqlite3.so", "world.db", "world.db-journal"):
            with self.subTest(name=name):
                path = self.package / name
                path.write_bytes(b"unexpected payload")
                with self.assertRaisesRegex(ClosureError, "Unexpected package entry"):
                    package_inventory(self.package)
                path.unlink()

    def test_sqlite_compile_options_are_private_and_exact(self):
        sqlite = next(item for item in self.commands if Path(item["file"]).name == "sqlite3.c")
        original = list(sqlite["arguments"])
        for arguments in ([item for item in original if item != "-DSQLITE_DQS=0"],
                          original + ["-DSQLITE_OMIT_WAL=1"], original + ["-DSQLITE_DQS=0"]):
            with self.subTest(arguments=arguments):
                sqlite["arguments"] = arguments
                self.write_evidence()
                with self.assertRaisesRegex(ClosureError, "Private SQLite compile definitions"):
                    compile_inventory(self.build, self.source, self.targets)
        sqlite["arguments"] = original
        self.commands[0]["arguments"].append("-DSQLITE_THREADSAFE=1")
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Private SQLite compile definitions"):
            compile_inventory(self.build, self.source, self.targets)

    def test_core_cannot_reach_storage_target_source_or_include(self):
        self.targets[0]["dependencies"] = [{"id": "storage"}]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "escaped its Server owner"):
            file_api(self.build)
        self.targets[0].pop("dependencies")
        self.targets[0]["sources"].append({"path": "server/storage/src/store.cpp", "compileGroupIndex": 0})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "storage source escaped"):
            file_api(self.build)
        self.targets[0]["sources"].pop()
        self.targets[0]["compileGroups"][0]["includes"].append({"path": str(self.source / "server/storage")})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "storage include escaped"):
            file_api(self.build)

    def test_private_sqlite_header_does_not_reach_core_or_server_consumers(self):
        header = self.build / "_deps/sqlite-src/sqlite3.h"
        for index in (0, 1):
            with self.subTest(owner=index):
                self.commands[index]["arguments"] += ["-I" + str(header.parent)]
                self.write_evidence()
                with self.assertRaisesRegex(ClosureError, "Private SQLite header escaped"):
                    compile_inventory(self.build, self.source, self.targets)
                self.commands[index]["arguments"].pop()
                evidence = f"unit.o: #deps 1, deps mtime 1 (VALID)\n    {header}\n"
                with self.assertRaisesRegex(ClosureError, "Private SQLite compiler-discovered input"):
                    compiler_headers(evidence, [{"path": self.commands[index]["file"], "object": "unit.o"}], self.source, self.build)

    def test_sqlite_shell_source_and_system_dynamic_library_are_forbidden(self):
        target = next(item for item in self.targets if item["name"] == "SharkSQLite")
        target["sources"].append({"path": str(self.build / "_deps/sqlite-src/shell.c"), "compileGroupIndex": 0})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Non-Core source"):
            file_api(self.build)
        with self.assertRaises(ClosureError):
            dynamic_needed("Dynamic section at offset 0x123 contains 1 entries:\n 0x1 (NEEDED) Shared library: [libsqlite3.so.0]\n")

    def test_tls_objects_must_be_actual_compiler_outputs_and_reject_injection(self):
        target = next(item for item in self.targets if item["name"] == "SharkTLS_builtin")
        target["artifacts"] = [{"path": "engine/core/src/core.cpp.o"}]
        with self.assertRaisesRegex(ClosureError, "TLS objects differ from actual compiler outputs"):
            compile_inventory(self.build, self.source, self.targets)
        crypto = next(item for item in self.targets if item["name"] == "SharkTLS_tfpsacrypto")
        crypto["sources"].append({"path": str(self.build / "hidden.o"), "isGenerated": True})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Unreviewed network object"):
            file_api(self.build)

    def test_tls_examples_and_missing_notices_do_not_enter_the_server_closure(self):
        path = self.build / "_deps/mbedtls-src/programs/ssl/example.c"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"int example;")
        tls = next(item for item in self.targets if item["name"] == "SharkTLS_mbedtls")
        tls["sources"].append({"path": str(path), "compileGroupIndex": 0})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Non-Core source"):
            file_api(self.build)
        for relative in MBEDTLS_LICENSE_FILES:
            notice = self.package / "licenses/MbedTLS" / relative
            data = notice.read_bytes()
            notice.unlink()
            with self.subTest(notice=relative), self.assertRaisesRegex(ClosureError, "Incomplete Server package"):
                package_inventory(self.package)
            notice.write_bytes(data)

    def test_accepts_glm_inline_headers_but_rejects_tampered_population(self):
        header = self.build / "_deps/glm-src/glm/detail/fixture.inl"
        self.targets[0]["compileGroups"][0]["includes"].append({"path": str(header.parents[2]), "isSystem": True})
        self.write_evidence()
        file_api(self.build)
        evidence = f"core.o: #deps 1, deps mtime 1 (VALID)\n    {header}\n"
        headers, system = compiler_headers(evidence, [{"object": "core.o"}], self.source, self.build)
        self.assertEqual([item["path"] for item in headers], [str(header)])
        self.assertEqual(system, [])
        header.write_bytes(b"tampered GLM header")
        with self.assertRaisesRegex(ClosureError, "Populated GLM source differs"):
            file_api(self.build)

    def test_missing_glm_source_archive_or_package_license_is_rejected(self):
        (self.build / "_deps/glm-src/copying.txt").unlink()
        with self.assertRaisesRegex(ClosureError, "GLM source inventory differs"):
            file_api(self.build)
        self.cache["entries"] = [item for item in self.cache["entries"] if item["name"] != "SHARK_GLM_ARCHIVE_INPUT"]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Missing locked Core"):
            file_api(self.build)
        (self.package / "licenses/GLM/copying.txt").unlink()
        with self.assertRaisesRegex(ClosureError, "Incomplete Server package"):
            package_inventory(self.package)

    def test_rejects_missing_entt_source_or_archive_evidence(self):
        (self.build / "_deps/entt-src/LICENSE").unlink()
        with self.assertRaisesRegex(ClosureError, "inventory differs"):
            file_api(self.build)
        self.cache["entries"] = [item for item in self.cache["entries"] if item["name"] != "SHARK_EnTT_ARCHIVE"]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Missing locked Core"):
            file_api(self.build)

    def test_rejects_authoring_include(self):
        (self.source / "engine_tools/include").mkdir(parents=True)
        self.targets[0]["compileGroups"][0]["includes"] = [{"path": str(self.source / "engine_tools/include")}]
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Non-Core include"):
            file_api(self.build)

    def test_rejects_generated_client_source(self):
        path = self.build / "engine/client/generated.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("int presentation;", encoding="utf-8")
        self.targets[0]["sources"].append({"path": str(path), "isGenerated": True, "compileGroupIndex": 0})
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "Non-Core source"):
            file_api(self.build)

    def test_rejects_prebuilt_objects_masquerading_as_core_sources(self):
        for name in ("hidden.o", "hidden.obj", "hidden.a", "hidden.lib"):
            with self.subTest(name=name):
                path = self.source / "server" / name
                path.write_bytes(b"prebuilt client object")
                self.targets[1]["sources"].append({"path": "server/" + name})
                self.write_evidence()
                with self.assertRaisesRegex(ClosureError, "prebuilt/noncompiled"):
                    file_api(self.build)
                self.targets[1]["sources"].pop()

    def test_generated_codemodel_paths_are_source_relative_for_nested_builds(self):
        build = self.source / "build/server-only"
        path = build / "engine/core/generated.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("int generated;", encoding="utf-8")
        self.assertEqual(codemodel_source({"path": "build/server-only/engine/core/generated.cpp", "isGenerated": True},
                                          self.source, build), path)

    def test_rejects_response_file_and_compiler_injection(self):
        for extra in ("@opaque.rsp", "-fplugin=hidden.so", "--sysroot=/other"):
            with self.subTest(extra=extra):
                self.commands[0]["arguments"].append(extra)
                self.write_evidence()
                with self.assertRaises(ClosureError):
                    compile_inventory(self.build, self.source, self.targets)
                self.commands[0]["arguments"].pop()

    def test_rejects_unreported_forced_header(self):
        path = self.source / "editor/hidden.hpp"
        path.parent.mkdir()
        path.write_text("// editor", encoding="utf-8")
        self.commands[0]["arguments"] += ["-include", str(path)]
        self.write_evidence()
        with self.assertRaises(ClosureError):
            compile_inventory(self.build, self.source, self.targets)

    def test_rejects_missing_or_duplicate_compile_units(self):
        self.commands.pop()
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "exactly cover"):
            compile_inventory(self.build, self.source, self.targets)
        self.commands.append(self.commands[0])
        self.write_evidence()
        with self.assertRaisesRegex(ClosureError, "exactly cover"):
            compile_inventory(self.build, self.source, self.targets)

    def test_accepts_only_server_package(self):
        self.assertEqual(package_inventory(self.package), self.package / "bin/Server")

    def test_rejects_package_extras_and_empty_directories(self):
        for relative, is_directory in (("bin/Editor", False), ("plugins", True)):
            with self.subTest(relative=relative):
                path = self.package / relative
                path.mkdir() if is_directory else path.write_text("extra", encoding="utf-8")
                with self.assertRaisesRegex(ClosureError, "Unexpected package entry"):
                    package_inventory(self.package)
                path.rmdir() if is_directory else path.unlink()

    def test_rejects_linked_package_binary(self):
        binary = self.package / "bin/Server"
        target = self.root / "external-Server"
        binary.rename(target)
        try:
            os.symlink(target, binary)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account cannot create symlinks; Linux closure gate exercises this")
            raise
        with self.assertRaisesRegex(ClosureError, "Linked input"):
            package_inventory(self.package)

    def test_rejects_non_elf_binary(self):
        (self.package / "bin/Server").write_text("pretend binary", encoding="utf-8")
        with self.assertRaisesRegex(ClosureError, "ELF binary"):
            package_inventory(self.package)

    def test_dynamic_inventory_rejects_unknown_libraries_and_search_paths(self):
        valid = "Dynamic section at offset 0x123 contains 1 entries:\n 0x1 (NEEDED) Shared library: [libc.so.6]\n"
        self.assertEqual(dynamic_needed(valid), ["libc.so.6"])
        for bad in (valid.replace("libc.so.6", "libvulkan.so.1"), valid + "0x2 (RUNPATH) [/custom]",
                    valid + "0x2 (RPATH) [/custom]", "There is no dynamic section in this file."):
            with self.subTest(output=bad), self.assertRaises(ClosureError):
                dynamic_needed(bad)

    def test_loader_rejects_missing_and_unreviewed_transitive_libraries(self):
        for line in ("libc.so.6 => not found", "libX11.so.6 => /lib/libX11.so.6 (0x123)",
                     "unexpected loader text", "libasan.so.8 => /lib/libasan.so.8 (0x123)"):
            with self.subTest(output=line), self.assertRaises(ClosureError):
                resolved_libraries(line, ["libc.so.6"], sanitizer="none")


if __name__ == "__main__":
    unittest.main()
