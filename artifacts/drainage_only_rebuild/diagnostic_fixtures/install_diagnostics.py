#!/usr/bin/env python3
"""Install opt-in quality fixtures only in a clean disposable City worktree."""
from pathlib import Path
import hashlib,json,subprocess
fixture=Path(__file__).resolve().parent
repo=fixture.parents[2]
status=subprocess.check_output(["git","status","--porcelain","--untracked-files=no"],cwd=repo,text=True)
if status.strip():
    raise SystemExit("Refusing to overwrite tracked edits; use a clean disposable worktree")
manifest=json.loads((fixture/"MANIFEST.json").read_text())
for entry in manifest["generation_inputs"]:
    if hashlib.sha256((repo/entry["path"]).read_bytes()).hexdigest()!=entry["sha256"]:
        raise SystemExit("Generation input differs from this diagnostic snapshot: "+entry["path"])
for entry in manifest["files"]:
    source=fixture/entry["path"]
    if hashlib.sha256(source.read_bytes()).hexdigest()!=entry["sha256"]:
        raise SystemExit("Fixture hash mismatch: "+entry["path"])
for entry in manifest["files"]:
    target=repo/entry["path"]
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes((fixture/entry["path"]).read_bytes())
print("Installed opt-in diagnostic fixtures with fresh build-input timestamps")
