#!/usr/bin/env python3
"""FBX class model end-to-end: an offline third-person game whose class wears an FBX user asset.

Run from the repo root: python3 -I client/tests/fbx_class_model_e2e.py [path/to/client_bin]
Checks the class-model probe (body=model, the FBX asset id) and writes a screenshot.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CLIENT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "cmake-build-debug" / "client_bin")
MAP = ROOT / "shared/tests/maps/v10/fbx_class_model.json"
ASSET = ROOT / "client/tests/data/fbx/sausage.fbx"


def main():
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))
        print(("PASS " if ok else "FAIL ") + what)

    check(CLIENT.exists(), f"client_bin exists ({CLIENT})")
    if not CLIENT.exists():
        return 1
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="kn-fbx-e2e-"))
    try:
        assets = ROOT / "assets/user-assets/999999001"
        assets.mkdir(parents=True, exist_ok=True)
        shutil.copy(ASSET, assets / "model.fbx")
        script = scratch / "input.txt"
        script.write_text(f"3000 models @fbx\n3000 shot {scratch / 'fbx.png'}\n3100 quit\n")
        env = dict(os.environ)
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        env.update(KRUNKNATIVE_HEADLESS="1", XDG_CONFIG_HOME=str(scratch / "cfg"),
                   KRUNKNATIVE_INPUT_SCRIPT=str(script), KRUNKNATIVE_FIXED_DELTA="1")
        run = subprocess.run([str(CLIENT), "--offline", str(MAP)], cwd=ROOT, env=env,
                             capture_output=True, text=True, timeout=120)
        log = run.stdout + run.stderr
        check("body=model,mdl=2,asset=999999001f" in log,
              "the FBX class model builds (models probe)")
        check("body=default" not in log and "body=none" not in log,
              "no default-body or empty fallback")
        check((scratch / "fbx.png").stat().st_size > 0 if (scratch / "fbx.png").exists() else False,
              f"third-person screenshot ({scratch / 'fbx.png'})")
    finally:
        shutil.rmtree(ROOT / "assets/user-assets/999999001", ignore_errors=True)
    failed = [what for ok, what in checks if not ok]
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
