#!/usr/bin/env python3
"""Expose the pinned driver's scene state and select our GLOBAL translator."""
import argparse
import hashlib
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ar", required=True)
parser.add_argument("--objcopy", required=True)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
if hashlib.sha256(args.source.read_bytes()).hexdigest() != "dff979edde7e1f56419c096a0cc439136e8d2daaea3224e7c37bf05100162bdc":
    raise ValueError("vitaGL archive differs from the pinned legacy softfp build; run tools/build_vita.py")
subprocess.run([
    args.objcopy, "--enable-deterministic-archives", "--weaken-symbol=glsl_translate_with_global",
    "--globalize-symbol=needs_end_scene", "--globalize-symbol=needs_scene_reset",
    str(args.source), str(args.output),
], check=True)
subprocess.run([args.ar, "sD", str(args.output)], check=True)
