"""Normalize inspection output for CMake's GNU-style PE import scanner."""
import subprocess
import sys


def inspect(command):
    # LLVM uses spaces where CMake's objdump parser expects tabs or no indent.
    # Keep interpretation and recursive dependency resolution inside CMake.
    with subprocess.Popen(command, stdout=subprocess.PIPE) as child:
        for line in child.stdout:
            sys.stdout.buffer.write(line.lstrip(b" \t").rstrip(b"\r\n") + b"\n")
        return child.wait()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(2)
    sys.exit(inspect(sys.argv[1:]))
