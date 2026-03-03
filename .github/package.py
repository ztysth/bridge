"""Validate and package native builds; no network access or credential handling."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ARCHITECTURES = {"linux": {"x86_64", "arm64", "riscv64"},
                 "windows": {"x86_64", "arm64"}, "macos": {"x86_64", "arm64"}}
EXTENSIONS = {"linux": ".deb", "windows": ".zip", "macos": ".dmg"}


def run(*args, env=None):
    subprocess.run(list(map(str, args)), check=True, env=env)


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


def smoke(executable):
    environment = dict(os.environ)
    for key in ("QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH", "LD_LIBRARY_PATH",
                "DYLD_LIBRARY_PATH"):
        environment.pop(key, None)
    environment.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
    run(executable, "--smoke-test", env=environment)


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
        run("cpack", "--config", build / "CPackConfig.cmake", "-B", output)
        candidates = list(output.glob("bridge_*.deb"))
        if len(candidates) != 1:
            raise ValueError("Expected exactly one DEB")
        asset = output / (stem + ".deb")
        candidates[0].rename(asset)
        # Exercise the actual installed package, with dependency resolution by apt.
        run("sudo", "apt-get", "install", "-y", asset.resolve())
        smoke("/usr/bin/bridge_gui")
        inventory = subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package} ${Version}\n", "libqt6*", "qml6-module-*", "libssl3t64"],
            text=True).splitlines()
    else:
        run("cmake", "--install", build, "--prefix", stage)
        if system == "windows":
            executable = stage / "bin" / "bridge_gui.exe"
            if not list(stage.rglob("Qt6Core.dll")) or not list(stage.rglob("qopensslbackend.dll")):
                raise ValueError("Qt/TLS runtime deployment is incomplete")
            (stage / "bridge.cmd").write_text('@echo off\n"%~dp0bin\\bridge_gui.exe" %*\n')
            notice_root = stage / "share" / "bridge"
        else:
            executable = stage / "bridge_gui.app" / "Contents" / "MacOS" / "bridge_gui"
            notice_root = stage / "bridge_gui.app" / "Contents" / "Resources" / "bridge"
            notice_root.mkdir(parents=True, exist_ok=True)
            for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
                shutil.copy2(ROOT / name, notice_root / name)
        for module in ("QtQuick", "QtQuick/Window", "QtQuick/Controls", "QtQuick/Layouts",
                       "QtQuick/Dialogs", "QtQml"):
            if not list(stage.rglob(module + "/qmldir")):
                raise ValueError(f"QML runtime deployment is incomplete: {module}")
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
    names = {p.name for p in directory.iterdir() if p.is_file()}
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
    elif args.action == "package":
        package(args.build.resolve(), args.output.resolve(), args.platform, args.arch, args.sdk,
                args.emulated)
    else:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        verify_assets(args.directory, version(), commit)


if __name__ == "__main__":
    main()
