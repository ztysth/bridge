import importlib.util
import json
from pathlib import Path
import platform
import shutil
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, call, patch

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[2] / ".github/package.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
probe_spec = importlib.util.spec_from_file_location(
    "openssl_probe", release.ROOT / ".github/openssl-tls-probe.py")
openssl_probe = importlib.util.module_from_spec(probe_spec)
with patch.dict(sys.modules, {"package": release}):
    probe_spec.loader.exec_module(openssl_probe)
PE_OBJDUMP = shutil.which("llvm-objdump") or shutil.which("llvm-objdump-18")


class ReleaseTests(unittest.TestCase):
    commit = "a" * 40

    def test_inspection_adapter_normalizes_output_and_preserves_failure(self):
        adapter = release.ROOT / "tools/objdump.py"
        for code in (0, 7):
            result = release.subprocess.run(
                [sys.executable, str(adapter), sys.executable, "-c",
                 f"import sys; sys.stdout.buffer.write(b'    DLL Name: required.dll\\r\\n'); sys.exit({code})"],
                capture_output=True, timeout=15)
            self.assertEqual(result.stdout, b"DLL Name: required.dll\n")
            self.assertEqual(result.returncode, code)
        result = release.subprocess.run([sys.executable, str(adapter)],
                                        capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)

    @unittest.skipUnless(sys.platform == "linux" and platform.machine() == "x86_64" and
                         PE_OBJDUMP and shutil.which("clang") and shutil.which("ld"),
                         "Real PE inspection fixture needs the native x86 LLVM/GNU tools")
    def test_cmake_resolves_real_pe_imports_through_the_adapter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "dependency.c").write_text("int required(void) { return 42; }\n")
            (root / "exports.def").write_text("EXPORTS\n required\n")
            (root / "main.c").write_text(
                "__declspec(dllimport) int required(void); void entry(void) { (void)required(); }\n")
            for command in (
                    ["clang", "--target=x86_64-pc-windows-msvc", "-c", "dependency.c", "-o", "dependency.obj"],
                    ["ld", "-mi386pep", "--dll", "--entry", "required", "--out-implib", "required.lib",
                     "-o", "required.dll", "dependency.obj", "exports.def"],
                    ["clang", "--target=x86_64-pc-windows-msvc", "-c", "main.c", "-o", "main.obj"],
                    ["ld", "-mi386pep", "--entry", "entry", "-o", "fixture.exe", "main.obj", "required.lib"]):
                release.subprocess.run(command, cwd=root, check=True,
                                       capture_output=True, timeout=15)
            driver = root / "inspect.cmake"
            driver.write_text('''
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "''' +
                              Path(sys.executable).as_posix() + ";" +
                              (release.ROOT / "tools/objdump.py").as_posix() + ";" +
                              Path(PE_OBJDUMP).as_posix() + '''")
file(GET_RUNTIME_DEPENDENCIES EXECUTABLES "${CMAKE_CURRENT_LIST_DIR}/fixture.exe"
  DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}"
  RESOLVED_DEPENDENCIES_VAR libraries UNRESOLVED_DEPENDENCIES_VAR missing)
if(missing OR NOT libraries STREQUAL "${CMAKE_CURRENT_LIST_DIR}/required.dll")
  message(FATAL_ERROR "PE import resolution failed: ${libraries}; ${missing}")
endif()
''')
            release.subprocess.run(["cmake", "-P", str(driver)], check=True,
                                   capture_output=True, text=True, timeout=15)

    def test_arm64_compiler_workaround_keeps_other_ports_optimized(self):
        with tempfile.TemporaryDirectory() as tmp:
            driver = Path(tmp) / "triplet.cmake"
            triplet = release.ROOT / "cmake/release-triplets/arm64-windows.cmake"
            driver.write_text('''
function(verify_port PORT)
  include("''' + triplet.as_posix() + '''")
  if(NOT VCPKG_CRT_LINKAGE STREQUAL "dynamic" OR NOT VCPKG_BUILD_TYPE STREQUAL "release")
    message(FATAL_ERROR "The workaround must preserve the release runtime")
  endif()
  if(PORT STREQUAL "openssl")
    if(NOT VCPKG_C_FLAGS_RELEASE STREQUAL "/Od" OR NOT VCPKG_CXX_FLAGS_RELEASE STREQUAL "/Od")
      message(FATAL_ERROR "The isolated SDK workaround is missing")
    endif()
  elseif(DEFINED VCPKG_C_FLAGS_RELEASE OR DEFINED VCPKG_CXX_FLAGS_RELEASE)
    message(FATAL_ERROR "Optimization was changed for an unrelated port")
  endif()
endfunction()
foreach(port openssl qtbase qtdeclarative catch2)
  verify_port("${port}")
endforeach()
''')
            release.subprocess.run(["cmake", "-P", str(driver)], check=True,
                                   capture_output=True, text=True, timeout=15)

    def test_independent_tls_probe_rejects_missing_sdk(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                openssl_probe.probe(Path(tmp))

    @unittest.skipUnless(sys.platform == "linux" and shutil.which("openssl"),
                         "The independent CLI harness is validated on Linux")
    def test_independent_tls_probe_uses_real_openssl_and_cleans_children(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            executable = sdk / "tools/openssl/openssl.exe"
            executable.parent.mkdir(parents=True)
            executable.symlink_to(shutil.which("openssl"))
            openssl_probe.probe(sdk)

    def test_windows_deployment_passes_sdk_paths_without_literal_quotes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            helper = release.ROOT / "cmake/DeployWindows.cmake"
            # Capture the generated script, then let CMake's real parser execute
            # it against a deployment-command seam that checks the received path.
            driver = root / "driver.cmake"
            driver.write_text('''
macro(qt_generate_deploy_script)
  qt6_generate_deploy_script(${ARGV})
endmacro()
function(qt6_generate_deploy_script)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET;OUTPUT_SCRIPT;CONTENT" "")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unexpected deployment arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()
  string(REPLACE "$<TARGET_FILE:fixture>" "fixture.exe" content "${arg_CONTENT}")
  string(REPLACE "$<TARGET_FILE_NAME:fixture>" "fixture.exe" content "${content}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deployment.cmake" "${content}")
  set(${arg_OUTPUT_SCRIPT} "${CMAKE_CURRENT_BINARY_DIR}/deployment.cmake" PARENT_SCOPE)
endfunction()
function(qt_deploy_qml_imports)
  set(bridge_plugins "fixture-plugin.dll" PARENT_SCOPE)
endfunction()
function(qt_deploy_runtime_dependencies)
  cmake_parse_arguments(arg "GENERATE_QT_CONF" "EXECUTABLE" "ADDITIONAL_MODULES;DEPLOY_TOOL_OPTIONS" ${ARGN})
  list(GET arg_DEPLOY_TOOL_OPTIONS 1 actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "SDK root argument was split or contains literal quotes")
  endif()
  if(NOT arg_ADDITIONAL_MODULES STREQUAL "fixture-plugin.dll" OR NOT arg_GENERATE_QT_CONF)
    message(FATAL_ERROR "QML runtime deployment was lost")
  endif()
endfunction()
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/unused-prefix")
set(QT_DEPLOY_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/stage")
# This seam checks script arguments and copy policy; the real scanner has its
# own compiled-binary fixture and native Windows deployment gate.
set(CMAKE_HOST_WIN32 FALSE)
set(ENV{SystemRoot} "${CMAKE_CURRENT_BINARY_DIR}/Windows")
include("''' + helper.as_posix() + '''")
function(file)
  if(ARGV0 STREQUAL "GET_RUNTIME_DEPENDENCIES")
    if(BRIDGE_MISSING_DEP)
      set(bridge_unresolved_dlls "missing.dll" PARENT_SCOPE)
    elseif(BRIDGE_FOREIGN_DEP)
      set(bridge_runtime_dlls "${CMAKE_CURRENT_BINARY_DIR}/sdk-sibling/foreign.dll" PARENT_SCOPE)
    else()
      set(bridge_runtime_dlls "${expected}/bin/nonqt.dll;${CMAKE_CURRENT_BINARY_DIR}/Windows/System32/os.dll" PARENT_SCOPE)
      set(bridge_unresolved_dlls "" PARENT_SCOPE)
    endif()
  else()
    _file(${ARGV})
    if(ARGV0 STREQUAL "REAL_PATH" OR ARGV0 STREQUAL "TO_CMAKE_PATH")
      set(${ARGV2} "${${ARGV2}}" PARENT_SCOPE)
    elseif(ARGV0 STREQUAL "GLOB" OR ARGV0 STREQUAL "GLOB_RECURSE")
      set(${ARGV1} "${${ARGV1}}" PARENT_SCOPE)
    endif()
  endif()
endfunction()
foreach(expected "${CMAKE_CURRENT_BINARY_DIR}/sdk" "${CMAKE_CURRENT_BINARY_DIR}/SDK with spaces")
  bridge_windows_deploy_script(fixture "${expected}" script)
  include("${script}")
endforeach()
''')
            for sdk_name in ("sdk", "SDK with spaces"):
                (root / sdk_name / "bin").mkdir(parents=True)
                (root / sdk_name / "bin/nonqt.dll").write_bytes(b"dependency")
                (root / sdk_name / "bin/unused.dll").write_bytes(b"unused")
            (root / "Windows/System32").mkdir(parents=True)
            (root / "Windows/System32/os.dll").write_bytes(b"system")
            (root / "sdk-sibling").mkdir()
            (root / "sdk-sibling/foreign.dll").write_bytes(b"foreign")
            release.subprocess.run(["cmake", "-P", str(driver)], cwd=root, check=True,
                                   capture_output=True, text=True, timeout=15)
            self.assertEqual((root / "stage/bin/nonqt.dll").read_bytes(), b"dependency")
            self.assertFalse((root / "stage/bin/unused.dll").exists())
            self.assertFalse((root / "stage/bin/os.dll").exists())
            for failure in ("BRIDGE_MISSING_DEP", "BRIDGE_FOREIGN_DEP"):
                with self.assertRaises(release.subprocess.CalledProcessError):
                    release.subprocess.run(["cmake", f"-D{failure}=ON", "-P", str(driver)],
                                           cwd=root, check=True, capture_output=True, text=True, timeout=15)

    def test_windows_child_error_mode_is_restored_after_success_or_failure(self):
        for error in (None, release.subprocess.CalledProcessError(5, ["fixture"])):
            kernel = Mock()
            kernel.GetErrorMode.return_value = 0x0020
            with patch.object(release.os, "name", "nt"), \
                    patch.object(release.ctypes, "WinDLL", return_value=kernel, create=True), \
                    patch.object(release.subprocess, "run", side_effect=error):
                if error is None:
                    release.run("fixture", timeout=15)
                else:
                    with self.assertRaises(release.subprocess.CalledProcessError):
                        release.run("fixture", timeout=15)
            self.assertEqual(kernel.SetErrorMode.call_args_list, [call(0x8023), call(0x0020)])

    @unittest.skipUnless(sys.platform == "linux" and shutil.which("cc"),
                         "The real resolver fixture needs a native Linux compiler")
    def test_deployment_resolves_transitive_sdk_libraries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sdk, stage = root / "SDK with spaces", root / "stage"
            for directory in (sdk / "bin", stage / "bin"):
                directory.mkdir(parents=True)
            source = root / "fixture.c"
            source.write_text("int required(void) { return 42; }\n")
            release.subprocess.run(["cc", "-shared", "-fPIC", "-nostdlib", str(source),
                                    "-Wl,-soname,required.dll", "-o", str(sdk / "bin/required.dll")],
                                   check=True, capture_output=True, timeout=15)
            source.write_text("extern int required(void); int wrapper(void) { return required(); }\n")
            release.subprocess.run(["cc", "-shared", "-fPIC", "-nostdlib", str(source),
                                    "-Wl,-soname,wrapper.dll", "-L" + str(sdk / "bin"),
                                    "-l:required.dll", "-o", str(stage / "bin/wrapper.dll")],
                                   check=True, capture_output=True, timeout=15)
            source.write_text("extern int wrapper(void); void _start(void) { (void)wrapper(); }\n")
            release.subprocess.run(["cc", "-nostdlib", str(source), "-L" + str(stage / "bin"),
                                    "-l:wrapper.dll", "-Wl,-rpath,$ORIGIN",
                                    "-Wl,-rpath-link," + str(sdk / "bin"),
                                    "-o", str(stage / "bin/fixture.exe")],
                                   check=True, capture_output=True, timeout=15)
            driver = root / "driver.cmake"
            driver.write_text('''
macro(qt_generate_deploy_script)
  qt6_generate_deploy_script(${ARGV})
endmacro()
function(qt6_generate_deploy_script)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET;OUTPUT_SCRIPT;CONTENT" "")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unexpected deployment arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()
  string(REPLACE "$<TARGET_FILE:fixture>" "fixture.exe" content "${arg_CONTENT}")
  string(REPLACE "$<TARGET_FILE_NAME:fixture>" "fixture.exe" content "${content}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deployment.cmake" "${content}")
  set(${arg_OUTPUT_SCRIPT} "${CMAKE_CURRENT_BINARY_DIR}/deployment.cmake" PARENT_SCOPE)
endfunction()
function(qt_deploy_qml_imports)
endfunction()
function(qt_deploy_runtime_dependencies)
endfunction()
set(QT_DEPLOY_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/stage")
set(ENV{SystemRoot} "${CMAKE_CURRENT_BINARY_DIR}/Windows")
include("''' + (release.ROOT / "cmake/DeployWindows.cmake").as_posix() + '''")
bridge_windows_deploy_script(fixture "${CMAKE_CURRENT_BINARY_DIR}/SDK with spaces" script)
include("${script}")
''')
            release.subprocess.run(["cmake", "-P", str(driver)], cwd=root, check=True,
                                   capture_output=True, text=True, timeout=15)
            self.assertEqual((stage / "bin/required.dll").read_bytes(),
                             (sdk / "bin/required.dll").read_bytes())

    def test_package_smoke_uses_host_backend_without_sdk_environment(self):
        sdk_variables = ("QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                         "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH")
        for host, backend in (("win32", "windows"), ("darwin", "cocoa"), ("linux", "offscreen")):
            with patch.object(release.sys, "platform", host), \
                    patch.dict(release.os.environ, {**{key: "fixture-sdk" for key in sdk_variables},
                                                   "SYSTEMROOT": "C:/Windows",
                                                   "PATH": "fixture-sdk"}), \
                    patch.object(release, "run") as command:
                release.smoke("fixture")
            self.assertEqual(command.call_args.args, ("fixture", "--smoke-test"))
            options = command.call_args.kwargs
            self.assertEqual(options["timeout"], 15)
            self.assertEqual(options["env"]["QT_QPA_PLATFORM"], backend)
            self.assertEqual(options["env"]["QT_QUICK_BACKEND"], "software")
            self.assertTrue(all(key not in options["env"] for key in sdk_variables))
            if host == "win32":
                self.assertNotIn("fixture-sdk", options["env"]["PATH"])

    def test_windows_transport_probe_is_removed_after_success_or_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build, stage = root / "build", root / "stage"
            build.mkdir()
            stage.mkdir()
            binary = "bridge_session_tool.exe"
            (build / binary).write_bytes(b"fixture")
            for error in (None, release.subprocess.CalledProcessError(5, ["fixture"])):
                def command(*args, **kwargs):
                    self.assertEqual(args, (release.sys.executable,
                                           release.ROOT / "tests/integration/process_pairing.py",
                                           stage / binary))
                    self.assertEqual((stage / binary).read_bytes(), b"fixture")
                    self.assertEqual(kwargs["timeout"], 60)
                    if error is not None:
                        raise error
                with patch.object(release, "run", side_effect=command):
                    if error is None:
                        release.probe_windows_transport(build, stage)
                    else:
                        with self.assertRaises(release.subprocess.CalledProcessError):
                            release.probe_windows_transport(build, stage)
                self.assertFalse((stage / binary).exists())
                self.assertTrue((build / binary).exists())
            (stage / binary).write_bytes(b"existing")
            with self.assertRaises(ValueError):
                release.probe_windows_transport(build, stage)
            self.assertEqual((stage / binary).read_bytes(), b"existing")

    def test_windows_failure_diagnostic_retains_bounded_native_debugger_call(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "bridge_integration_tests.exe").write_bytes(b"fixture")
            debugger = root / "Windows Kits/10/Debuggers/arm64/cdb.exe"
            debugger.parent.mkdir(parents=True)
            debugger.write_bytes(b"fixture")
            error = release.subprocess.CalledProcessError(0xC0000005, ["fixture"])
            with patch.dict(release.os.environ, {"ProgramFiles(x86)": str(root),
                                               "BRIDGE_CI_ARCH": "arm64"}), \
                    patch.object(release, "run", side_effect=[error, None, None, None]) as command:
                release.diagnose_windows(root)
            self.assertEqual(command.call_args_list[0].kwargs["timeout"], 30)
            group_call = command.call_args_list[1]
            self.assertEqual(group_call.kwargs["timeout"], 30)
            self.assertIn("OPENSSL_CONF", group_call.kwargs["env"])
            self.assertFalse(Path(group_call.kwargs["env"]["OPENSSL_CONF"]).exists())
            self.assertEqual(command.call_args.args[0], debugger)
            self.assertIn(".ecxr; kp", command.call_args.args[2])
            self.assertEqual(command.call_args.kwargs["timeout"], 90)

    def make_assets(self, root):
        for system, architectures in release.ARCHITECTURES.items():
            for arch in architectures:
                stem = f"bridge-0.1.0-{system}-{arch}"
                metadata = {"version": "0.1.0", "platform": system, "architecture": arch,
                            "source_commit": self.commit, "emulated": arch == "riscv64",
                            "dependencies": ["Qt 6"]}
                for suffix in (release.EXTENSIONS[system], ".json"):
                    path = root / (stem + suffix)
                    path.write_bytes(json.dumps(metadata).encode() if suffix == ".json" else b"fixture")
                    path.with_name(path.name + ".sha256").write_text(
                        f"{release.checksum(path)}  {path.name}\n")

    def test_platform_architecture_headers(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "app"
            for arch, machine in (("x86_64", 62), ("arm64", 183), ("riscv64", 243)):
                data = bytearray(64)
                data[:6] = b"\x7fELF\x02\x01"
                struct.pack_into("<H", data, 18, machine)
                binary.write_bytes(data)
                release.validate_binary(binary, "linux", arch)
                with self.assertRaises(ValueError):
                    release.validate_binary(binary, "linux", "arm64" if arch != "arm64" else "x86_64")
            for system, machines in (("windows", (0x8664, 0xAA64)),
                                     ("macos", (0x1000007, 0x100000C))):
                for arch, machine in zip(("x86_64", "arm64"), machines):
                    data = bytearray(70)
                    if system == "windows":
                        data[:2] = b"MZ"
                        struct.pack_into("<I", data, 60, 64)
                        data[64:68] = b"PE\0\0"
                        struct.pack_into("<H", data, 68, machine)
                    else:
                        data[:4] = b"\xcf\xfa\xed\xfe"
                        struct.pack_into("<I", data, 4, machine)
                    binary.write_bytes(data)
                    release.validate_binary(binary, system, arch)
            for data in (b"", b"bad" * 64, b"MZ" + b"\0" * 62):
                binary.write_bytes(data)
                with self.assertRaises(ValueError):
                    release.validate_binary(binary, "windows", "x86_64")
            with self.assertRaises(ValueError):
                release.validate_binary(binary, "macos", "riscv64")

    def test_qml_runtime_requires_used_modules_without_window_compatibility_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            modules = ("QtQuick", "QtQuick/Controls", "QtQuick/Layouts",
                       "QtQuick/Dialogs", "QtQml", "QtQuick/Templates")
            for module in modules:
                qmldir = stage / "qml" / module / "qmldir"
                qmldir.parent.mkdir(parents=True, exist_ok=True)
                qmldir.write_text("module " + module.replace("/", ".") + "\n")
            release.validate_qml_runtime(stage)
            self.assertFalse((stage / "qml/QtQuick/Window").exists())
            for module in modules:
                qmldir = stage / "qml" / module / "qmldir"
                content = qmldir.read_text()
                qmldir.unlink()
                with self.assertRaises(ValueError):
                    release.validate_qml_runtime(stage)
                qmldir.write_text(content)

    def test_release_requires_every_asset_and_valid_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(ValueError):
                release.verify_assets(root, "0.1.0", self.commit)
            self.make_assets(root)
            release.verify_assets(root, "0.1.0", self.commit)
            self.assertEqual(len((root / "SHA256SUMS").read_text().splitlines()), 14)
            victim = root / "bridge-0.1.0-linux-arm64.deb"
            victim.write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                release.verify_assets(root, "0.1.0", self.commit)

    def test_linux_packaging_keeps_cpack_scratch_outside_release_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            output = root / "dist"
            build.mkdir()
            header = bytearray(64)
            header[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", header, 18, 62)
            (build / "bridge_gui").write_bytes(header)

            def command(*args):
                if args[0] == "cpack":
                    directory = Path(args[-1])
                    (directory / "_CPack_Packages").mkdir(parents=True)
                    (directory / "bridge_0.1.0_amd64.deb").write_bytes(b"package")
                else:
                    self.assertEqual(args[:4], ("sudo", "apt-get", "install", "-y"))
                    self.assertEqual(args[4], (output / "bridge-0.1.0-linux-x86_64.deb").resolve())

            with patch.object(release, "run", side_effect=command), \
                    patch.object(release, "smoke") as smoke, \
                    patch.object(release.subprocess, "check_output",
                                 side_effect=["Qt 6.4.2\n", self.commit + "\n"]):
                release.package(build, output, "linux", "x86_64")
            smoke.assert_called_once_with("/usr/bin/bridge_gui")
            self.assertTrue((build / "release-stage/_CPack_Packages").is_dir())
            self.assertEqual({p.name for p in output.iterdir()}, {
                "bridge-0.1.0-linux-x86_64.deb", "bridge-0.1.0-linux-x86_64.deb.sha256",
                "bridge-0.1.0-linux-x86_64.json", "bridge-0.1.0-linux-x86_64.json.sha256"})

    def test_rejects_mixed_sources_metadata_and_extra_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_assets(root)
            for commit in ("b" * 40, "not-a-commit"):
                with self.assertRaises(ValueError):
                    release.verify_assets(root, "0.1.0", commit)
            inventory = root / "bridge-0.1.0-linux-arm64.json"
            for content in ("[]", "{}", "x" * (1024 * 1024 + 1)):
                inventory.write_text(content)
                with self.assertRaises(ValueError):
                    release.verify_assets(root, "0.1.0", self.commit)
            self.make_assets(root)
            scratch = root / "_CPack_Packages"
            scratch.mkdir()
            with self.assertRaises(ValueError):
                release.verify_assets(root, "0.1.0", self.commit)
            scratch.rmdir()
            (root / "unintended-secret.txt").write_text("fixture")
            with self.assertRaises(ValueError):
                release.verify_assets(root, "0.1.0", self.commit)
            (root / "unintended-secret.txt").unlink()
            receipt = root / "bridge-0.1.0-linux-arm64.deb.sha256"
            receipt.write_text("a" * 64 + "  ../escape\n")
            with self.assertRaises(ValueError):
                release.verify_assets(root, "0.1.0", self.commit)


if __name__ == "__main__":
    unittest.main()
