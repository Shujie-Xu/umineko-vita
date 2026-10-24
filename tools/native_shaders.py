#!/usr/bin/env python3
"""Validate the complete GLES corpus and native matrix-table shader binaries."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


def fnv64(data):
    result = 14695981039346656037
    for byte in data:
        result = ((result ^ byte) * 1099511628211) & 0xffffffffffffffff
    return result


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--resources", type=Path, required=True)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
text = args.resources.read_text()
names = dict(re.findall(r'static const char filename_(\d+)\[\] = "([^"]+)";', text))
sources = {}
for index, values in re.findall(r"resource_(\d+)_gles_buffer\[\] = \{([^}]+)\}", text):
    data = bytes(map(int, re.findall(r"\d+", values)))
    if not data.endswith(b"\0"):
        raise ValueError(f"Unterminated shader: {names[index]}")
    sources[names[index]] = data[:-1]
gpu = (root / "vendor/sdl-gpu/include/SDL_gpu_GLES_2.h").read_text().replace("\\\n", "")
for kind in ("TEXTURED_VERTEX", "UNTEXTURED_VERTEX", "TEXTURED_FRAGMENT", "UNTEXTURED_FRAGMENT"):
    match = re.search(r"#define GPU_DEFAULT_" + kind + r'_SHADER_SOURCE\s+("(?:[^"\\]|\\.)*")', gpu)
    sources["sdl-" + kind.lower()] = ast.literal_eval(match[1]).encode()
manifest = json.loads((args.directory / "manifest.json").read_text())
if manifest["format"] != "ons-gxp-matrix-table-v1" or manifest["abi"] != args.directory.name:
    raise ValueError("Shader ABI manifest does not match its directory")
expected = {}
for name, data in sources.items():
    # SDL_gpu's GetShaderSource_RW retains comment delimiters only.
    if b"#include" in data:
        raise ValueError(f"Unexpected shader include: {name}")
    data = re.sub(
        rb"(#[^\n]*(?:\n|$))|(/\*.*?\*/)|(//[^\n]*(?:\n|$))",
        lambda match: match[1] or (b"/**/" if match[2] else b"//\n"),
        data, flags=re.S,
    )
    shader_type = 35633 if name.endswith((".vert", "_vertex")) else 35632
    expected[f"{shader_type}-{fnv64(data):016x}.bin"] = name
if set(expected) != set(manifest["shaders"]):
    raise ValueError(f"Shader corpus mismatch: {set(expected) ^ set(manifest['shaders'])}")
if set(expected) != {path.name for path in args.directory.glob("*.bin")}:
    raise ValueError("Shader files do not match the complete corpus")
for name, source in expected.items():
    entry = manifest["shaders"][name]
    data = (args.directory / name).read_bytes()
    if entry["source"] != source or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise ValueError(f"Shader manifest mismatch: {name}")
    magic, version, kind, low, high, size, crc, reserved = struct.unpack_from("<8I", data)
    payload = data[32:]
    if (magic, version, reserved) != (0x31534756, 1, 0) or size != len(payload):
        raise ValueError(f"Invalid shader header: {name}")
    if name != f"{kind}-{(high << 32) | low:016x}.bin" or zlib.crc32(payload) != crc:
        raise ValueError(f"Invalid shader hash or CRC: {name}")
    count, = struct.unpack_from("<I", payload)
    offset = (count + 1) * 4
    if offset + 32 > len(payload) or payload[offset:offset + 3] != b"GXP":
        raise ValueError(f"Invalid GXP matrix table: {name}")
print(f"Validated {len(expected)} native shaders against the complete GLES corpus")
