"""P4 : vrai ZIP, coupure du récepteur, reprise TLS depuis le checkpoint et SHA-256."""

import hashlib
import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import zipfile


class Process:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.output = []
        self.errors = []
        self.threads = [threading.Thread(target=self.read, args=(self.process.stdout, self.output, True), daemon=True),
                        threading.Thread(target=self.read, args=(self.process.stderr, self.errors, False), daemon=True)]
        for thread in self.threads:
            thread.start()

    def read(self, stream, destination, notify):
        for line in stream:
            destination.append(line)
            if notify:
                self.lines.put(line.strip())
        if notify:
            self.lines.put(None)

    def until(self, predicate):
        while True:
            line = self.lines.get(timeout=60)
            assert line is not None, "Process ended before expected event: " + "".join(self.errors)
            if predicate(line):
                return line

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        for thread in self.threads:
            thread.join(timeout=2)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    server_executable, client_executable = sys.argv[1:3]
    large = "--large" in sys.argv[3:]
    size = (1024 if large else 64) * 1024 * 1024
    with tempfile.TemporaryDirectory(prefix="blendnet-transfer-") as temporary:
        root = pathlib.Path(temporary)
        archive = root / "source.zip"
        # ZIP_STORED : archive réellement volumineuse, construite en flux sans lib runtime.
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as project:
            with project.open("payload.bin", "w", force_zip64=True) as output:
                block = bytes(range(256)) * 4096
                for _ in range(size // len(block)):
                    output.write(block)
        total = archive.stat().st_size
        identity = root / "identity"
        received = root / "received"
        server_command = [server_executable, "--identity-dir", str(identity), "--transfer-dir", str(received),
                          "--port", "0", "--seconds", "90"]
        first = Process(server_command)
        client = second = None
        try:
            listening = first.until(lambda line: line.startswith("LISTENING "))
            port = int(listening.split()[1])
            token = (identity / "pairing-token.txt").read_text()
            settings = root / "settings.json"
            settings.write_text(json.dumps({"knownSources": [{"host": "127.0.0.1", "port": port,
                                      "token": token, "certificateSha256": None}]}))
            os.chmod(settings, 0o600)
            client = Process([client_executable, "--config", str(settings), "--connect", "0",
                              "--send-file", str(archive), "--file-id", "large_RESULT_ZIP", "--kind", "RESULT_ZIP",
                              "--transfer-dir", str(root / "client-received"), "--seconds", "90", "--exit-after-transfer"])
            cutoff = total // (2 if large else 4)
            client.until(lambda line: line.startswith("TRANSFER SEND ") and int(line.split()[2]) >= cutoff)
            first.stop()  # Vraie fermeture du processus récepteur, pas un reset de mock.
            metadata = list(received.glob("*.json"))
            assert len(metadata) == 1
            saved = json.loads(metadata[0].read_text())
            assert not saved["complete"], "Transfer finished before the simulated interruption"
            checkpoint = min(saved["nextSeq"] * saved["offer"]["chunkSize"], total)
            assert cutoff <= checkpoint < total
            server_command[server_command.index("--port") + 1] = str(port)
            second = Process(server_command)
            second.until(lambda line: line.startswith("LISTENING "))
            client.until(lambda line: line == "TRANSFER_DONE SEND")
            assert client.process.wait(timeout=5) == 0, "".join(client.errors)
            assert any(line.startswith("RECONNECTING ") for line in client.output)
            resumed = [int(line.split()[4]) for line in client.output if line.startswith("TRANSFER SEND ")]
            assert max(resumed) >= checkpoint, "Whole archive was retransmitted instead of resuming"
            final = list(received.glob("*.zip"))
            assert len(final) == 1
            assert final[0].stat().st_size == total
            assert sha256(final[0]) == sha256(archive), "Received archive differs from the source"
            assert not list(received.glob("*.part"))
            with zipfile.ZipFile(final[0]) as project:
                assert project.getinfo("payload.bin").file_size == size
            print(f"Validated ZIP: {total} bytes; resumed at {max(resumed)} bytes")
        finally:
            for process in [client, first, second]:
                if process:
                    process.stop()
            token_path = identity / "pairing-token.txt"
            if token_path.exists():
                token = token_path.read_text()
                for process in [client, first, second]:
                    if process:
                        assert token not in "".join(process.output + process.errors)


if __name__ == "__main__":
    main()
