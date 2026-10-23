"""P6 opt-in : Blender officiel vérifié, trois frames CPU et benchmark persisté."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

from cli_transfer import Process


def png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    size = struct.unpack(">II", data[16:24])
    offset = 8
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        chunk = data[offset + 4:offset + 8 + length]
        assert zlib.crc32(chunk) == struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
        offset += length + 12
    assert offset == len(data)
    return size


def main():
    client, server, version, cache = sys.argv[1:]
    cache = str(pathlib.Path(cache).resolve())
    with tempfile.TemporaryDirectory(prefix="blendnet-blender-") as temporary:
        root = pathlib.Path(temporary)
        settings = root / "settings.json"
        settings.write_text(json.dumps({"language": "fr", "workDirectory": "preserve-work"}))
        base = [client, "--config", str(settings), "--blender-cache", cache]

        def run(args, success=True):
            result = subprocess.run(base + args, capture_output=True, text=True, timeout=600)
            assert (result.returncode == 0) == success, result.stdout + result.stderr
            return result

        smoke = root / "smoke"
        result = run(["--blender-smoke", version, "--output-dir", str(smoke), "--backend", "CPU"])
        assert "RENDER_DONE 3" in result.stdout
        assert (smoke / "scene.blend").stat().st_size > 0
        images = sorted((smoke / "frames").glob("*.png"))
        assert len(images) == 3
        assert all(png(path) == (32, 32) for path in images)
        hardware = run(["--probe-blender", version, "--backend", "CPU"])
        backends = next(line.split()[1:] for line in hardware.stdout.splitlines() if line.startswith("BACKENDS "))
        assert "CPU" in backends
        unavailable = next((backend for backend in ["OPTIX", "CUDA", "HIP", "ONEAPI", "METAL"] if backend not in backends), None)
        if unavailable:
            rejected = run(["--probe-blender", version, "--backend", unavailable], success=False)
            assert "backend is not available" in rejected.stderr
        stepped = root / "stepped"
        run(["--render-local", version, "--scene", str(smoke / "scene.blend"), "--output-dir", str(stepped),
             "--frame-start", "1", "--frame-end", "5", "--frame-step", "2", "--backend", "CPU"])
        assert sorted(path.name for path in stepped.glob("*.png")) == ["frame_0001.png", "frame_0003.png", "frame_0005.png"]
        run(["--render-local", version, "--scene", str(smoke / "scene.blend"), "--output-dir", str(root / "negative"),
             "--frame-start", "-2", "--frame-end", "0", "--backend", "CPU"], success=False)
        assert not list((root / "negative").glob("*.png"))
        benchmark = run(["--benchmark-blender", version, "--backend", "CPU"])
        line = next(line for line in benchmark.stdout.splitlines() if line.startswith("BENCHMARK "))
        score = int(line.split()[1])
        stored = json.loads(settings.read_text())
        assert score > 0 and stored["benchmarkScore"] == score
        assert stored["workDirectory"] == "preserve-work" and stored["language"] == "fr"

        # Vérifier le parcours réel GPU_INFO → TLS, sans le mode simulate-jobs.
        identity = root / "identity"
        source = Process([server, "--identity-dir", str(identity), "--port", "0", "--seconds", "90"])
        try:
            port = int(source.until(lambda line: line.startswith("LISTENING ")).split()[1])
            token = (identity / "pairing-token.txt").read_text()
            stored["knownSources"] = [{"host": "127.0.0.1", "port": port, "token": token, "certificateSha256": None}]
            settings.write_text(json.dumps(stored))
            connected = run(["--use-blender", version, "--backend", "CPU", "--connect", "0", "--seconds", "1"])
            assert "READY TLSv1." in connected.stdout
            source.until(lambda line: line == "CLIENT_READY")
            assert token not in connected.stdout + connected.stderr
        finally:
            source.stop()
        print(f"P6: real Blender {version}; 3/3 PNG 32x32; stepped chunk; benchmark {score}; real GPU_INFO over TLS")


if __name__ == "__main__":
    main()
