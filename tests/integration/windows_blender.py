"""Exercise the installed CLI with a fresh portable Blender and no configured local executable."""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def inventory(directory):
    # The official ZIP includes paths beyond MAX_PATH; keep diagnostics usable on Windows.
    root = directory.resolve()
    if os.name == "nt" and not str(root).startswith("\\\\?\\"):
        root = pathlib.Path("\\\\?\\" + str(root))
    entries = {}
    for path in root.rglob("*"):
        name = path.relative_to(root).as_posix()
        if path.is_dir():
            entries[name] = "directory"
        else:
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            entries[name] = digest.hexdigest()
    return entries


def check_unchanged(before, after, mode):
    added = sorted(after.keys() - before.keys())
    removed = sorted(before.keys() - after.keys())
    changed = sorted(name for name in before.keys() & after.keys() if before[name] != after[name])
    for label, names in [("added", added), ("removed", removed), ("changed", changed)]:
        if names:
            print(f"Cache {label} after {mode}: {len(names)} entries", file=sys.stderr, flush=True)
            print("\n".join(names[:25]), file=sys.stderr, flush=True)
    assert not (added or removed or changed), f"Verified Blender installation changed after {mode}"


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
        baseline = None
        content = None
        for mode in ["--install-blender", "--probe-blender", "--benchmark-blender"]:
            result = subprocess.run(base + [mode, version, "--backend", "CPU"], cwd=root,
                                    text=True, encoding="utf-8", errors="replace",
                                    capture_output=True, timeout=900)
            print(result.stdout, flush=True)
            print(result.stderr, file=sys.stderr, flush=True)
            if baseline is not None:
                check_unchanged(baseline, inventory(content), mode)
            assert result.returncode == 0, f"{mode}: exit {result.returncode}"
            if mode == "--install-blender":
                manifests = list(cache.glob("blender-*/manifest.json"))
                assert len(manifests) == 1, manifests
                content = manifests[0].parent / "content"
                baseline = inventory(content)
            if mode == "--probe-blender":
                assert any(line.startswith("BACKENDS ") and "CPU" in line.split()
                           for line in result.stdout.splitlines())
        stored = json.loads(settings.read_text(encoding="utf-8"))
        assert stored["benchmarkScore"] > 0
        assert stored["blenderExecutable"] == ""
    print("Fresh portable Blender: installation, CPU detection and benchmark passed")


if __name__ == "__main__":
    main()
