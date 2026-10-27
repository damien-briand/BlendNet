"""Start the actual AppImage runtime without FUSE or a build-tree library path."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--gui", action="store_true")
    args = parser.parse_args()
    image = args.image.resolve()
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    env["APPIMAGE_EXTRACT_AND_RUN"] = "1"
    with tempfile.TemporaryDirectory(prefix="blendnet-appimage-") as temporary:
        work = pathlib.Path(temporary)
        result = subprocess.run([str(image), "--version"], cwd=work, env=env,
                                text=True, capture_output=True, timeout=45)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "BlendNet" in result.stdout
        if args.gui:
            result = subprocess.run([str(image), "--smoke-test", "--config",
                                     str(work / "settings.json"), "--screenshot",
                                     str(work / "smoke.ppm")], cwd=work, env=env,
                                    text=True, capture_output=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            assert (work / "smoke.ppm").read_bytes().startswith(b"P6\n")
            assert (work / "settings.json").is_file()
    print("AppImage runtime and application startup verified")


if __name__ == "__main__":
    main()
