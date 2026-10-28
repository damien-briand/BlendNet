"""Exercise the installed CLI with a fresh portable Blender and no configured local executable."""

import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    version = sys.argv[2] if len(sys.argv) > 2 else "4.2.1"
    with tempfile.TemporaryDirectory(prefix="blendnet clean install ") as temporary:
        root = pathlib.Path(temporary)
        settings = root / "settings.json"
        settings.write_text(json.dumps({"logLevel": "debug", "blenderExecutable": ""}),
                            encoding="utf-8")
        cache = root / "blender cache"
        base = [str(executable), "--config", str(settings), "--blender-cache", str(cache)]
        for mode in ["--install-blender", "--probe-blender", "--benchmark-blender"]:
            result = subprocess.run(base + [mode, version, "--backend", "CPU"], cwd=root,
                                    text=True, encoding="utf-8", errors="replace",
                                    capture_output=True, timeout=900)
            print(result.stdout, flush=True)
            print(result.stderr, file=sys.stderr, flush=True)
            assert result.returncode == 0, f"{mode}: exit {result.returncode}"
            if mode == "--probe-blender":
                assert any(line.startswith("BACKENDS ") and "CPU" in line.split()
                           for line in result.stdout.splitlines())
        stored = json.loads(settings.read_text(encoding="utf-8"))
        assert stored["benchmarkScore"] > 0
        assert stored["blenderExecutable"] == ""
    print("Fresh portable Blender: installation, CPU detection and benchmark passed")


if __name__ == "__main__":
    main()
