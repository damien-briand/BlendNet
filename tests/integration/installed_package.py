"""Check a relocated installation, with optional three-frame OpenGL startup."""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("prefix", type=pathlib.Path)
    parser.add_argument("--gui", action="store_true", help="Requires a display and OpenGL 3.3")
    args = parser.parse_args()
    root = args.prefix.resolve()
    suffix = ".exe" if os.name == "nt" else ""
    candidates = [(root / "bin", root / "share/blendnet"),
                  (root / "usr/bin", root / "usr/share/blendnet"),
                  (root / "BlendNet.app/Contents/MacOS", root / "BlendNet.app/Contents/Resources")]
    layout = next(((b, r) for b, r in candidates
                   if (b / ("blendnet-cli" + suffix)).is_file()), None)
    if layout is None:
        checked = ", ".join(str(b / ("blendnet-cli" + suffix)) for b, _ in candidates)
        parser.error(f"Installation incomplete under {root}: executable not found; checked {checked}")
    binary, resources = layout
    assert (resources / "fonts/DejaVuSans.ttf").stat().st_size > 10000
    assert (resources / "licenses/LICENSE").is_file()
    assert (resources / "README.md").is_file()
    env = os.environ.copy()
    for key in ["LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"]:
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="blendnet-installed-") as temporary:
        work = pathlib.Path(temporary)
        for name in ["blendnet-cli", "blendnet-server-cli", "blendnet"]:
            executable = binary / (name + suffix)
            if name == "blendnet" and sys.platform == "darwin":
                executable = binary / "BlendNet"
            if not executable.exists() and name == "blendnet":
                continue  # Headless package.
            for option in ["--version", "--help"]:
                result = subprocess.run([str(executable), option], cwd=work, env=env,
                                        text=True, capture_output=True, timeout=30)
                assert result.returncode == 0, (executable, result.stdout, result.stderr)
                assert "blendnet" in result.stdout.lower(), (executable, result.stdout)
            if sys.platform.startswith("linux"):
                linked = subprocess.run(["ldd", str(executable)], capture_output=True, text=True,
                                        env=env, timeout=10)
                assert "not found" not in linked.stdout, linked.stdout
                assert "/_deps/" not in linked.stdout, linked.stdout
            if args.gui and name == "blendnet":
                result = subprocess.run([str(executable), "--smoke-test", "--config",
                                         str(work / "settings.json"), "--screenshot",
                                         str(work / "smoke.ppm")], cwd=work, env=env,
                                        text=True, capture_output=True, timeout=45)
                assert result.returncode == 0, result.stdout + result.stderr
                assert (work / "smoke.ppm").read_bytes().startswith(b"P6\n")
                assert (work / "settings.json").is_file()
    print("Relocated package: binaries, loader dependencies, resources and startup verified")


if __name__ == "__main__":
    main()
