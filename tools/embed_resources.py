#!/usr/bin/env python3
"""Run the pinned upstream resource compiler with Vita's GLES selection."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--tool", required=True)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
resources = args.source / "Resources"
paths = set()
for pattern in ("Shaders/*.frag", "Shaders/*.vert", "Images/*.png"):
    for path in resources.glob(pattern):
        if not path.name.startswith("._"):
            paths.add(path.with_name(path.name.replace(".gl.", ".").replace(".gles.", ".")))
args.output.parent.mkdir(parents=True, exist_ok=True)
command = [args.tool]
for path in sorted(paths):
    command.extend((str(path), path.name))
subprocess.run([*command, str(args.output)], check=True)
text = args.output.read_text()
text = text.replace("#if defined(IOS)", "#if defined(VITA) || defined(IOS)")
args.output.write_text(text)
