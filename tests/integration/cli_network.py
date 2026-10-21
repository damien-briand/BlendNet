"""P3 : vrais processus serveur/CLI, TLS et pin persisté, sans Blender ni GPU."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    server_executable, client_executable = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="blendnet-cli-") as temporary:
        root = pathlib.Path(temporary)
        identity = root / "identity"
        server = subprocess.Popen(
            [server_executable, "--identity-dir", str(identity), "--port", "0", "--seconds", "10"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            line = server.stdout.readline().strip()
            assert line.startswith("LISTENING "), "Server did not start"
            port = int(line.split()[1])
            token = (identity / "pairing-token.txt").read_text()
            settings = root / "settings.json"
            settings.write_text(json.dumps({"knownSources": [{
                "host": "127.0.0.1", "port": port, "token": token, "certificateSha256": None,
            }]}))
            os.chmod(settings, 0o600)
            command = [client_executable, "--config", str(settings), "--connect", "0", "--seconds", "1"]
            connected = subprocess.run(command, capture_output=True, text=True, timeout=5)
            assert connected.returncode == 0, connected.stderr
            assert "READY TLSv1." in connected.stdout, "Missing TLS registration"
            stored = json.loads(settings.read_text())
            fingerprint = stored["knownSources"][0]["certificateSha256"]
            assert len(fingerprint) == 64
            assert token not in connected.stdout + connected.stderr, "Token appeared in client logs"

            stored["knownSources"][0]["token"] = "invalid-token"
            settings.write_text(json.dumps(stored))
            rejected = subprocess.run(command, capture_output=True, text=True, timeout=5)
            assert rejected.returncode == 1
            assert "pairing token rejected" in rejected.stderr
            assert "invalid-token" not in rejected.stdout + rejected.stderr
            assert json.loads(settings.read_text())["knownSources"][0]["certificateSha256"] == fingerprint
        finally:
            server.terminate()
            try:
                output, errors = server.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                output, errors = server.communicate(timeout=3)
            if (identity / "pairing-token.txt").exists():
                assert (identity / "pairing-token.txt").read_text() not in output + errors


if __name__ == "__main__":
    main()
