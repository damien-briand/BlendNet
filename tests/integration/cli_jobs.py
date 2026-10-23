"""P5 : trois vrais processus clients simulés, mort d'un client et séquence complète."""

import json
import os
import pathlib
import struct
import sys
import tempfile
import zipfile
import zlib

from cli_transfer import Process


def main():
    server_executable, client_executable = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="blendnet-jobs-") as temporary:
        root = pathlib.Path(temporary)
        with zipfile.ZipFile(root / "project.zip", "w") as project:
            project.writestr("scene.blend", b"P5: simulated project, no Blender execution")
        job = {"id": "demo-job", "projectName": "P5 integration", "projectZip": "project.zip",
               "outputDirectory": "output", "frameStart": 1, "frameEnd": 60, "chunkSize": 5}
        (root / "job.json").write_text(json.dumps(job))
        identity = root / "identity"
        source = Process([server_executable, "--identity-dir", str(identity), "--port", "0",
                          "--publish-job", str(root / "job.json"), "--wait-clients", "3",
                          "--exit-after-job", "--seconds", "30", "--offer-timeout-ms", "3000",
                          "--task-timeout-ms", "3000"])
        clients = []
        try:
            port = int(source.until(lambda line: line.startswith("LISTENING ")).split()[1])
            token = (identity / "pairing-token.txt").read_text()
            for index in range(3):
                directory = root / f"client-{index}"
                directory.mkdir()
                settings = directory / "settings.json"
                settings.write_text(json.dumps({"knownSources": [{"host": "127.0.0.1", "port": port,
                                      "token": token, "certificateSha256": None}]}))
                os.chmod(settings, 0o600)
                clients.append(Process([client_executable, "--config", str(settings), "--connect", "0",
                                        "--simulate-jobs", "--machine-name", f"worker-{index}",
                                        "--frame-delay-ms", "250" if index == 0 else "40",
                                        "--seconds", "30"]))
            clients[0].until(lambda line: line == "SIM_TASK_STARTED")
            clients[0].process.kill()  # Mort brutale pendant le premier chunk.
            clients[0].process.wait(timeout=5)
            done = source.until(lambda line: line.startswith("JOB Done "))
            _, _, valid, total, retries = done.split()
            assert int(valid) == int(total) == 60, done
            assert int(retries) >= 1, "Dead client's chunk was not redistributed"
            assert source.process.wait(timeout=5) == 0, "".join(source.errors)
            images = sorted((root / "output").glob("frame_*.png"))
            assert len(images) == 60
            for frame in range(1, 61):
                data = (root / "output" / f"frame_{frame:04}.png").read_bytes()
                assert data[:8] == b"\x89PNG\r\n\x1a\n"
                assert struct.unpack(">II", data[16:24]) == (1, 1)
                position = 8
                while position < len(data):
                    length = struct.unpack(">I", data[position:position + 4])[0]
                    chunk = data[position + 4:position + 8 + length]
                    assert zlib.crc32(chunk) == struct.unpack(">I", data[position + 8 + length:position + 12 + length])[0]
                    position += 12 + length
            for client in clients[1:]:
                assert any(line.strip() == "SIM_TASK_STARTED" for line in client.output)
                assert sum(line.strip() == "TRANSFER_DONE RECEIVE" for line in client.output) == 1, "Project sent more than once"
            print(f"P5: 60/60 verified frames; {retries} retry/retries after a killed client")
        finally:
            for process in clients + [source]:
                process.stop()
            if (identity / "pairing-token.txt").exists():
                token = (identity / "pairing-token.txt").read_text()
                for process in clients + [source]:
                    assert token not in "".join(process.output + process.errors)


if __name__ == "__main__":
    main()
