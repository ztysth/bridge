"""Diagnostic-only TLS loopback using the pinned OpenSSL CLI, without Qt."""
from pathlib import Path
import os
import socket
import subprocess
import sys
import tempfile
import time
from package import child_error_mode, run


def probe(sdk):
    openssl = sdk / "tools" / "openssl" / "openssl.exe"
    if not openssl.is_file():
        raise ValueError("The pinned OpenSSL CLI is unavailable")
    run(openssl, "version", timeout=5)
    with child_error_mode(), tempfile.TemporaryDirectory(prefix="bridge-openssl-") as tmp:
        root = Path(tmp)
        key, cert = root / "key.pem", root / "cert.pem"
        config = root / "openssl.cnf"
        config.write_text("[req]\nprompt=no\ndistinguished_name=dn\n[dn]\nCN=bridge.local\n")
        environment = dict(os.environ)
        environment["OPENSSL_CONF"] = str(config)
        run(openssl, "req", "-new", "-x509", "-newkey", "ec", "-noenc",
            "-pkeyopt", "ec_paramgen_curve:P-256", "-keyout", key, "-out", cert,
            "-subj", "/CN=bridge.local", "-days", "1", env=environment, timeout=10)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with subprocess.Popen([str(openssl), "s_server", "-accept", f"127.0.0.1:{port}",
                               "-cert", str(cert), "-key", str(key), "-CAfile", str(cert),
                               "-Verify", "1", "-verify_return_error", "-tls1_3", "-www",
                               "-naccept", "1"], stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              env=environment) as server:
            try:
                time.sleep(0.5)
                with subprocess.Popen([str(openssl), "s_client", "-connect", f"127.0.0.1:{port}",
                                       "-cert", str(cert), "-key", str(key), "-CAfile", str(cert),
                                       "-verify_return_error", "-verify_hostname", "bridge.local",
                                       "-tls1_3", "-quiet", "-no_ign_eof"], stdin=subprocess.PIPE,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                      env=environment) as client:
                    try:
                        client.communicate(b"GET / HTTP/1.0\r\n\r\n", timeout=10)
                        print(f"Independent OpenSSL client exit: {client.returncode}", flush=True)
                    finally:
                        if client.poll() is None:
                            client.kill()
                        client.wait(timeout=5)
                server.communicate(timeout=10)
                print(f"Independent OpenSSL server exit: {server.returncode}", flush=True)
                if client.returncode or server.returncode:
                    raise RuntimeError("Independent OpenSSL TLS handshake failed")
            finally:
                if server.poll() is None:
                    server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    probe(Path(sys.argv[1]))
