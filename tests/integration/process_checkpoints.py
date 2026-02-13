"""Real abrupt-exit/restart checkpoint recovery; no network or GUI automation."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1]).resolve()
    memcheck = []
    if os.environ.get("BRIDGE_VALGRIND"):
        memcheck = [os.environ.get("BRIDGE_VALGRIND_BIN", "valgrind"),
                    "--error-exitcode=99", "--leak-check=full",
                    "--errors-for-leak-kinds=definite,indirect,possible"]
    chunk_size = 1048576
    expected = bytes([23]) * chunk_size + bytes([7, 9, 11]) + bytes(28)
    for action, durable in [("crash", chunk_size), ("crash-data", 0), ("crash-torn", 0),
                            ("crash-record", chunk_size), ("crash-publish", len(expected))]:
        with tempfile.TemporaryDirectory(prefix="bridge-restart-") as directory:
            root = Path(directory)
            # An intentional hard-exit process is not a leak-test target. The
            # production operations also have normally destructed Memcheck tests.
            crashed = subprocess.run([binary, action, root], capture_output=True, timeout=20)
            assert crashed.returncode == 70, (action, crashed.returncode, crashed.stderr)
            assert (root / "received.bin").exists() == (action == "crash-publish")
            resumed = subprocess.run([*memcheck, binary, "resume", root],
                                     capture_output=True, timeout=90 if memcheck else 20)
            assert resumed.returncode == 0, (action, resumed.returncode, resumed.stderr)
            assert resumed.stdout == f"resumed_bytes={durable}\n".encode(), resumed.stdout
            assert (root / "received.bin").read_bytes() == expected
            assert [path.name for path in root.iterdir()] == ["received.bin"]
    print("five abrupt-exit checkpoint recovery cases passed")


if __name__ == "__main__":
    main()
