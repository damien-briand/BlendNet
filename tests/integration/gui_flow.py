"""P8: ordinary ImGui mouse/key input in two GUI processes; verified real Blender over TLS."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import time
import uuid

from cli_transfer import Process
from blender_render import png
class GuiProcess(Process):
    def until(self, predicate):
        while True:
            line=self.lines.get(timeout=180)
            assert line is not None, "GUI ended before expected event: "+"".join(self.errors)
            if predicate(line): return line


def main():
    gui, cli, version, cache, artifacts = sys.argv[1:]
    root = pathlib.Path(artifacts).resolve() / ("run-" + uuid.uuid4().hex[:8])
    root.mkdir(parents=True)
    cache = str(pathlib.Path(cache).resolve())
    project = root / "project"
    made = subprocess.run([cli, "--config", str(root / "fixture.json"), "--blender-cache", cache,
                           "--blender-smoke", version, "--output-dir", str(project)],
                          text=True, capture_output=True, timeout=180)
    assert made.returncode == 0, made.stdout + made.stderr
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    def settings(role):
        folder = root / role
        folder.mkdir()
        path = folder / "settings.json"
        path.write_text(json.dumps({"blenderCacheDirectory": cache, "blenderVersion": version,
                                   "workDirectory": str(folder / "work"), "serverPort": port,
                                   "benchmarkPrompted": False}))
        os.chmod(path, 0o600)
        return path

    def script(role, steps):
        path = root / (role + "-steps.json")
        path.write_text(json.dumps({"steps": steps}))
        os.chmod(path, 0o600)
        return str(path)

    def click(key): return {"id": key}
    def wait(key): return {"id": "wait." + key, "action": "wait"}
    def text(key, value): return {"id": key, "action": "text", "value": value}

    source_config = settings("source")
    source_script = script("source", [click("benchmark.later"), wait("idle"), click("nav.settings"),
        click("server.start"), wait("server"), click("nav.projects"),
        text("library.folder", str(project)), click("library.add"), wait("projects"), click("project.0"),
        wait("project"), text("job.chunk", "2"), wait("clients"), click("job.share"), wait("done"),
        click("nav.settings"), click("settings.general"), click("language.en"), click("settings.save"),
        wait("idle"), click("nav.job"), {"id": "wait.done", "action": "delay", "value": "90"}])
    source = GuiProcess([gui, "--config", str(source_config), "--ui-test", source_script,
                      "--screenshot", str(root / "source.ppm")])
    client = None
    try:
        source.until(lambda line: line == "UI_STEP wait.server")
        token = (root / "source/identity/pairing-token.txt").read_text()
        client_config = settings("client")
        client_script = script("client", [click("benchmark.later"), wait("idle"), click("role.client"),
            click("nav.settings"), click("source.add"), text("source.host", "127.0.0.1"),
            text("source.port", str(port)), text("source.token", token), click("settings.save"),
            wait("idle"), click("source.connect"), wait("connected"),
            {"id": "offer.accept", "action": "key", "value": "Enter"}, wait("twoChunks"),
            click("nav.renders"), {"id": "wait.completed", "action": "delay", "value": "120"}])
        client = GuiProcess([gui, "--config", str(client_config), "--ui-test", client_script,
                          "--screenshot", str(root / "client.ppm")])
        source.until(lambda line: line == "UI_STEP wait.done")
        assert source.process.wait(timeout=30) == 0, "".join(source.errors)
        assert client.process.wait(timeout=30) == 0, "".join(client.errors)
        images = sorted((root / "source/work/renders/scene").glob("*.png"))
        assert len(images) == 3, images
        assert all(png(path) == (32, 32) for path in images)
        stored = json.loads(client_config.read_text())
        assert stored["knownSources"][0]["certificateSha256"]
        assert stored["benchmarkPrompted"]
        assert json.loads(source_config.read_text())["language"] == "en"
        assert not list((root / "client/work").rglob("result.zip")), "Result retained after final ACK"
        for process in [source, client]:
            assert token not in "".join(process.output + process.errors)
        assert (root / "source.ppm").exists() and (root / "client.ppm").exists()
        print("P8 GUI: folder -> project -> Share Render; configured source -> popup accepted with Enter; 2 real chunks; 3/3 PNG; FR/EN; ACK cleanup")
        print("Artifacts:", root)
    except Exception:
        print("Source UI:", "".join(source.output), "".join(source.errors))
        if client: print("Client UI:", "".join(client.output), "".join(client.errors))
        raise
    finally:
        for process in [source, client]:
            if process: process.stop()
        # The input replay contains a test pairing token; do not retain it as a report artifact.
        for path in root.glob("*-steps.json"):
            path.unlink()


if __name__ == "__main__":
    main()
