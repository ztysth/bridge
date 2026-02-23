"""Two real processes: controller compares fingerprints before granting consent."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def run_case(binary, reject=False):
    with tempfile.TemporaryDirectory(prefix="bridge-pair-") as directory:
        root = Path(directory)
        children = []
        messages = queue.Queue()
        workers = []
        def start(role, port):
            command = [binary, role, "127.0.0.1", str(port), "--commands", str(root / role)]
            if os.environ.get("BRIDGE_VALGRIND") == "1":
                command = [os.environ.get("BRIDGE_VALGRIND_BIN", "valgrind"), "--error-exitcode=99", "--leak-check=full",
                           "--errors-for-leak-kinds=definite,indirect", *command]
            log = (root / (role + ".log")).open("w+")
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=log, text=True)
            children.append((child, log))
            def read():
                for line in child.stdout:
                    messages.put((role, json.loads(line)))
            worker = threading.Thread(target=read)
            worker.start()
            workers.append(worker)
            return child
        try:
            server = start("receive", 0)
            role, ready = messages.get(timeout=20)
            assert role == "receive" and ready["event"] == "ready", ready
            client = start("connect", ready["port"])
            fingerprints = {}
            deadline = time.monotonic() + 25
            while len(fingerprints) != 2:
                role, event = messages.get(timeout=max(0.1, deadline - time.monotonic()))
                assert event["event"] == "fingerprint", event
                fingerprints[role] = event["value"]
            assert fingerprints["receive"] == fingerprints["connect"]
            assert len(fingerprints["receive"]) == 64
            # No consent yet: neither process may report completion or exit.
            assert server.poll() is None and client.poll() is None
            (root / "connect").write_text("confirm")
            (root / "receive").write_text("reject" if reject else "confirm")
            expected = 2 if reject else 0
            results = [child.wait(timeout=20) for child, _ in children]
            assert results == [expected, expected], results
            for _, log in children:
                log.flush(); log.seek(0); content = log.read()
                assert fingerprints["receive"] not in content, "fingerprint leaked to diagnostic log"
            print("two-process rejection passed" if reject else "two-process pairing passed")
        finally:
            for child, _ in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
                if child.stdout:
                    child.stdout.close()
            for worker in workers:
                worker.join(timeout=5)
            for _, log in children:
                log.close()


if __name__ == "__main__":
    run_case(sys.argv[1])
    run_case(sys.argv[1], reject=True)
