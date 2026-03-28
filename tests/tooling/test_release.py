import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[2] / ".github/package.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    commit = "a" * 40

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
                    self.assertEqual(args[4], output / "bridge-0.1.0-linux-x86_64.deb")

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
