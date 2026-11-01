"""Run the production event journal against POSIX files and injected I/O faults.

Requires Linux, GCC and GNU ld (also works inside WSL). No board, private
configuration, cloud access or existing journal is used.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/d121/overlay/application/rt-thread/helloworld/cloud"


def main():
    with tempfile.TemporaryDirectory(prefix="d12x-outbox-build-") as directory:
        executable = Path(directory) / "test-outbox"
        command = shlex.split(os.environ.get("CC", "gcc")) + [
            "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(SOURCE), str(SOURCE / "cloud_outbox.c"),
            str(ROOT / "tests/test_outbox.c"),
            "-Wl,--wrap=fsync", "-Wl,--wrap=open", "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
