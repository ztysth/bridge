"""Validate and package native builds; no network access or credential handling."""
import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARCHITECTURES = {"linux": {"x86_64", "arm64", "riscv64"},
                 "windows": {"x86_64", "arm64"}, "macos": {"x86_64", "arm64"}}
EXTENSIONS = {"linux": ".deb", "windows": ".zip", "macos": ".dmg"}


@contextmanager
def child_error_mode():
    set_error_mode = None
    previous_mode = 0
    if os.name == "nt":
        # Loader dialogs otherwise hide missing DLL failures on unattended runners.
        # This helper is single-threaded; descendants inherit its process error mode.
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetErrorMode.argtypes = []
        kernel.GetErrorMode.restype = ctypes.c_uint
        set_error_mode = kernel.SetErrorMode
        set_error_mode.argtypes = [ctypes.c_uint]
        set_error_mode.restype = ctypes.c_uint
        previous_mode = kernel.GetErrorMode()
        set_error_mode(previous_mode | 0x0001 | 0x0002 | 0x8000)
    try:
        yield
    finally:
        if set_error_mode is not None:
            set_error_mode(previous_mode)


def run(*args, env=None, timeout=None):
    with child_error_mode():
        subprocess.run(list(map(str, args)), check=True, env=env, timeout=timeout)


def version(root=ROOT):
    match = re.search(r"project\(bridge VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b",
                      (root / "CMakeLists.txt").read_text())
    if not match or json.loads((root / "vcpkg.json").read_text())["version-string"] != match[1]:
        raise ValueError("CMake and manifest versions must agree")
    return match[1]


def validate_binary(path, system, architecture):
    if architecture not in ARCHITECTURES.get(system, set()):
        raise ValueError("Unsupported platform/architecture")
    with Path(path).open("rb") as binary:
        header = binary.read(64)
        if len(header) < 64:
            raise ValueError("Truncated executable")
        if system == "linux":
            if header[:6] != b"\x7fELF\x02\x01":
                raise ValueError("Expected a 64-bit little-endian ELF")
            machine = struct.unpack_from("<H", header, 18)[0]
            expected = {"x86_64": 62, "arm64": 183, "riscv64": 243}[architecture]
        elif system == "windows":
            offset = struct.unpack_from("<I", header, 60)[0]
            if header[:2] != b"MZ" or offset < 64 or offset > 1024 * 1024:
                raise ValueError("Invalid PE header offset")
            binary.seek(offset)
            pe = binary.read(6)
            if len(pe) != 6 or pe[:4] != b"PE\0\0":
                raise ValueError("Invalid PE header")
            machine = struct.unpack_from("<H", pe, 4)[0]
            expected = {"x86_64": 0x8664, "arm64": 0xAA64}[architecture]
        else:
            if header[:4] != b"\xcf\xfa\xed\xfe":
                raise ValueError("Expected a single-architecture 64-bit Mach-O")
            machine = struct.unpack_from("<I", header, 4)[0]
            expected = {"x86_64": 0x1000007, "arm64": 0x100000C}[architecture]
        if machine != expected:
            raise ValueError("Executable architecture does not match the asset label")


def checksum(path):
    with Path(path).open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def runtime_environment():
    environment = dict(os.environ)
    for key in ("QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH", "LD_LIBRARY_PATH",
                "DYLD_LIBRARY_PATH"):
        environment.pop(key, None)
    platform = {"win32": "windows", "darwin": "cocoa"}.get(sys.platform, "offscreen")
    environment.update(QT_QPA_PLATFORM=platform, QT_QUICK_BACKEND="software")
    if sys.platform == "win32":
        # Do not accidentally resolve an omitted DLL from the SDK or host OpenSSL.
        # dict(os.environ) loses Windows' case-insensitive lookup semantics.
        windows_root = next((value for key, value in environment.items()
                             if key.upper() == "SYSTEMROOT"), None)
        if windows_root is None:
            raise ValueError("Windows system root is missing from the environment")
        windows = Path(windows_root)
        environment["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    return environment


def smoke(executable):
    run(executable, "--smoke-test", env=runtime_environment(), timeout=15)


def probe_windows_transport(build, executable_directory):
    # Exercise production TLS with deployed DLLs, then remove the test executable.
    # The session tool has only application dependencies; a Catch2 executable
    # would require test-framework DLLs that correctly do not ship in the app.
    probe = executable_directory / "bridge_session_tool.exe"
    if probe.exists():
        raise ValueError("Unexpected transport probe in the package")
    shutil.copy2(build / probe.name, probe)
    try:
        run(sys.executable, ROOT / "tests/integration/process_pairing.py", probe,
            env=runtime_environment(), timeout=60)
    finally:
        probe.unlink()


def diagnose_windows(build):
    probe = build / "bridge_integration_tests.exe"
    if not probe.is_file():
        print("No integration executable was built; inspect the compiler failure.")
        return
    # A separate diagnostic rerun cannot turn the failed CTest step green.
    try:
        run(probe, "--success", timeout=30)
    except subprocess.CalledProcessError as error:
        print(f"Integration diagnostic exit code: {error.returncode}", flush=True)
    # Isolate whether OpenSSL's default key-exchange groups cause this native
    # crash. This configuration is diagnostic-only and is never packaged.
    with tempfile.TemporaryDirectory(prefix="bridge-tls-diagnostic-") as directory:
        config = Path(directory) / "openssl.cnf"
        config.write_text("openssl_conf=bridge_diagnostic\n[bridge_diagnostic]\n"
                          "ssl_conf=tls_settings\n[tls_settings]\n"
                          "system_default=group_probe\n[group_probe]\nGroups=P-256\n")
        environment = dict(os.environ)
        environment["OPENSSL_CONF"] = str(config)
        print("Testing TLS with the P-256 group in an isolated diagnostic process.", flush=True)
        try:
            run(probe, "explicit local receive and bilateral TLS pairing complete cleanly",
                env=environment, timeout=30)
        except subprocess.CalledProcessError as error:
            print(f"P-256 group diagnostic exit code: {error.returncode}", flush=True)
    try:
        run(build / "bridge_openssl_probe.exe", timeout=15)
    except subprocess.CalledProcessError as error:
        print(f"Independent OpenSSL diagnostic exit code: {error.returncode}", flush=True)
    architecture = "arm64" if os.environ.get("BRIDGE_CI_ARCH") == "arm64" else "x64"
    debugger = (Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits" / "10" /
                "Debuggers" / architecture / "cdb.exe")
    if not debugger.is_file():
        print(f"The runner has no {architecture} Windows SDK command-line debugger.")
        return
    run(debugger, "-c", "sxe av; g; .exr -1; .ecxr; kp 20; q", probe,
        "explicit local receive and bilateral TLS pairing complete cleanly", timeout=90)


def validate_qml_runtime(stage):
    # Main.qml imports and their shared QML/Controls runtime dependencies.
    # QtQuick.Window is a compatibility import, not a required separate module.
    for module in ("QtQuick", "QtQuick/Controls", "QtQuick/Controls/Basic", "QtQuick/Layouts",
                   "QtQuick/Dialogs", "QtQml", "QtQuick/Templates"):
        if not list(stage.rglob(module + "/qmldir")):
            raise ValueError(f"QML runtime deployment is incomplete: {module}")


def package(build, output, system, architecture, sdk=None, emulated=False):
    release_version = version()
    output.mkdir(parents=True, exist_ok=True)
    stage = build / "release-stage"
    if stage.exists():
        raise ValueError("Use a fresh build directory for packaging")
    stem = f"bridge-{release_version}-{system}-{architecture}"
    if system == "linux":
        executable = build / "bridge_gui"
        validate_binary(executable, system, architecture)
        # CPack creates a recursive _CPack_Packages tree beside the installer.
        # Keep that build output out of the directory uploaded as release assets.
        run("cpack", "--config", build / "CPackConfig.cmake", "-B", stage)
        candidates = list(stage.glob("bridge_*.deb"))
        if len(candidates) != 1:
            raise ValueError("Expected exactly one DEB")
        asset = output / (stem + ".deb")
        # The emulated guest mounts build and release output on separate volumes.
        shutil.copy2(candidates[0], asset)
        # Exercise the actual installed package, with dependency resolution by apt.
        run("sudo", "apt-get", "install", "-y", asset.resolve())
        smoke("/usr/bin/bridge_gui")
        inventory = subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package} ${Version}\n", "libqt6*", "qml6-module-*", "libssl3t64", "libutf8proc3"],
            text=True).splitlines()
    else:
        run("cmake", "--install", build, "--prefix", stage)
        if system == "windows":
            executable = stage / "bin" / "bridge_gui.exe"
            if (not list(stage.rglob("Qt6Core.dll")) or
                    not list(stage.rglob("qopensslbackend.dll")) or
                    not list(executable.parent.glob("libssl*.dll")) or
                    not list(executable.parent.glob("libcrypto*.dll"))):
                raise ValueError("Qt/TLS runtime deployment is incomplete")
            probe_windows_transport(build, executable.parent)
            (stage / "bridge.cmd").write_text('@echo off\n"%~dp0bin\\bridge_gui.exe" %*\n')
            notice_root = stage / "share" / "bridge"
        else:
            executable = stage / "bridge_gui.app" / "Contents" / "MacOS" / "bridge_gui"
            notice_root = stage / "bridge_gui.app" / "Contents" / "Resources" / "bridge"
            notice_root.mkdir(parents=True, exist_ok=True)
            for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
                shutil.copy2(ROOT / name, notice_root / name)
        validate_qml_runtime(stage)
        validate_binary(executable, system, architecture)
        if sdk is None:
            raise ValueError("An installed vcpkg SDK is required")
        copyrights = list((sdk / "share").glob("*/copyright"))
        if not copyrights:
            raise ValueError("Dependency notices are missing")
        for source in copyrights:
            target = notice_root / "licenses" / source.parent.name / "copyright"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        inventory = subprocess.check_output(
            [str(Path(os.environ["VCPKG_ROOT"]) / ("vcpkg.exe" if os.name == "nt" else "vcpkg")),
             "list", "--x-json", f"--x-install-root={sdk.parent}"], text=True)
        if system == "macos":
            run("codesign", "--force", "--deep", "--sign", "-", stage / "bridge_gui.app")
        smoke(executable)
        asset = output / (stem + EXTENSIONS[system])
        if system == "windows":
            shutil.make_archive(str(asset.with_suffix("")), "zip", stage)
        else:
            run("hdiutil", "create", "-volname", f"bridge {release_version}", "-srcfolder", stage,
                "-ov", "-format", "UDZO", asset)
    metadata = output / (stem + ".json")
    metadata.write_text(json.dumps({"version": release_version, "platform": system,
                                    "architecture": architecture, "emulated": emulated,
                                    "source_commit": subprocess.check_output(
                                        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                                    "dependencies": inventory}, indent=2) + "\n")
    for path in (asset, metadata):
        path.with_name(path.name + ".sha256").write_text(f"{checksum(path)}  {path.name}\n")


def verify_assets(directory, release_version, source_commit):
    expected = {f"bridge-{release_version}-{system}-{arch}{EXTENSIONS[system]}"
                for system, architectures in ARCHITECTURES.items() for arch in architectures}
    entries = list(directory.iterdir())
    if any(not p.is_file() or p.is_symlink() for p in entries):
        raise ValueError("Unexpected release directory or symlink")
    names = {p.name for p in entries}
    if not expected <= names:
        raise ValueError("The release is missing required platform packages")
    allowed = {"SHA256SUMS"}
    for name in expected:
        metadata_name = Path(name).stem + ".json"
        allowed.update((name, metadata_name, name + ".sha256", metadata_name + ".sha256"))
    if names - allowed:
        raise ValueError("Unexpected release files")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("Invalid source commit")
    checksums = []
    for name in sorted(expected):
        metadata_path = directory / (Path(name).stem + ".json")
        if metadata_path.stat().st_size > 1024 * 1024:
            raise ValueError("Oversized package inventory")
        metadata = json.loads(metadata_path.read_text())
        if not isinstance(metadata, dict):
            raise ValueError("Invalid package inventory")
        system = metadata.get("platform")
        arch = metadata.get("architecture")
        if (system not in ARCHITECTURES or arch not in ARCHITECTURES[system]
                or metadata.get("version") != release_version
                or metadata.get("source_commit") != source_commit
                or type(metadata.get("emulated")) is not bool
                or not metadata.get("dependencies")
                or name != f"bridge-{release_version}-{system}-{arch}{EXTENSIONS[system]}"):
            raise ValueError("Package inventory does not match this release")
        for item in (name, Path(name).stem + ".json"):
            path = directory / item
            receipt = directory / (item + ".sha256")
            content = receipt.read_text()
            if not re.fullmatch(r"[0-9a-f]{64}  " + re.escape(item) + r"\n", content):
                raise ValueError("Invalid checksum receipt")
            if content[:64] != checksum(path):
                raise ValueError("Checksum mismatch")
            checksums.append(content)
    (directory / "SHA256SUMS").write_text("".join(checksums))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)
    check = sub.add_parser("version")
    check.add_argument("--tag")
    test = sub.add_parser("test")
    test.add_argument("--build", type=Path, required=True)
    diagnostic = sub.add_parser("diagnose-windows")
    diagnostic.add_argument("--build", type=Path, required=True)
    pack = sub.add_parser("package")
    pack.add_argument("--build", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    pack.add_argument("--platform", choices=ARCHITECTURES, required=True)
    pack.add_argument("--arch", choices=("x86_64", "arm64", "riscv64"), required=True)
    pack.add_argument("--sdk", type=Path)
    pack.add_argument("--emulated", action="store_true")
    verify = sub.add_parser("verify")
    verify.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "version":
        value = version()
        if args.tag is not None and args.tag != "v" + value:
            raise ValueError("Release tag must match the source version")
        print(value)
    elif args.action == "test":
        run("ctest", "--test-dir", args.build, "--output-on-failure")
    elif args.action == "diagnose-windows":
        diagnose_windows(args.build)
    elif args.action == "package":
        package(args.build.resolve(), args.output.resolve(), args.platform, args.arch, args.sdk,
                args.emulated)
    else:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        verify_assets(args.directory, version(), commit)


if __name__ == "__main__":
    main()
