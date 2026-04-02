# bridge

Transfer files and folders between nearby computers over Wi-Fi or Ethernet.
Bridge uses TLS 1.3, verifies received data, and saves checkpoints for interrupted
transfers. Files and folders can be dragged into the window.

## Usage

1. Select **Receive** on the destination computer, choose a folder and network
   address, then click **Start receiving**.
2. On the other computer, drop a file or folder into **Send** and enter the
   receiver's address and port.
3. Compare the fingerprint on both screens and confirm the match. Click
   **Accept** on the receiver to begin.
4. Use **Pause** and **Continue** during a transfer. After a disconnection, keep
   the sender open and select **Resume interrupted transfer** on both sides.

Existing destination files are preserved. Symlinks and unsafe paths are rejected.
Names currently use portable ASCII characters. Windows file transfer is still
under development.

## Download

[Releases](https://github.com/ztysth/bridge/releases)

Windows and macOS builds target x86-64 and ARM64. Linux builds target x86-64,
ARM64 and experimental RISC-V 64. Linux packages require Ubuntu 24.04; macOS
packages require macOS 14 or later.

## Build

Requires a C++23 compiler, CMake 3.25+, Ninja and vcpkg. Set `VCPKG_ROOT` to your
vcpkg checkout; dependency versions are pinned in `vcpkg.json`. With GCC 13's
standard library, use GCC 13+ or Clang 19+. macOS builds require Xcode 26+.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The executable is built under `../bridge-build/debug/`. Installed Qt 6, OpenSSL 3
and Catch2 3 SDKs can use the `debug-system` preset instead.

## License

[BSD-3-Clause](LICENSE). Third-party components retain their own licenses; see
[notices](THIRD_PARTY_NOTICES.md).
