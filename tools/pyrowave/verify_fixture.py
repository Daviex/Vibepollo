"""Verify a synthetic fixture's checksums and run its offline GPU decoder."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--decoder", type=Path, required=True)
    args = parser.parse_args()
    fixture = args.fixture_dir.resolve()
    manifest = json.loads((fixture / "manifest.json").read_text())
    assert manifest["fixture_version"] == 1 and manifest["api_version"] == "0.5.0"
    assert manifest["bitstream_revision"] == "d2997ac172bdc00e29c58e3f2938acb7e94580bf"
    assert manifest["profile"] == "sdr-bt709-full-left-420"
    frame = (fixture / "frame.pwvf").read_bytes()
    assert len(frame) == manifest["pwvf_size"] and sha256(frame) == manifest["pwvf_sha256"]
    width, height = manifest["width"], manifest["height"]
    assert 64 <= width <= 4096 and 64 <= height <= 4096 and width % 2 == height % 2 == 0
    assert manifest["synthetic_frame_index"] == 0 and manifest["source_layout"] == "NV12"
    pattern = manifest["synthetic_pattern"]
    assert pattern in ("constant", "gradient")
    y_plane = bytearray(64 if pattern == "constant" else 32 + ((x // 4 + y // 4) % 192) for y in range(height) for x in range(width))
    uv_plane = bytearray()
    for y in range(height // 2):
        for x in range(width // 2):
            uv_plane.extend((96, 160) if pattern == "constant" else (96 + (x // 16) % 64, 96 + (y // 16) % 64))
    assert sha256(y_plane + uv_plane) == manifest["source_sha256"]
    count = len(manifest["native_packets"])
    assert struct.unpack_from("<I", frame, 12)[0] == count
    native_offset = 32
    for i, packet in enumerate(manifest["native_packets"]):
        name = packet["file"]
        assert Path(name).name == name
        data = (fixture / name).read_bytes()
        assert len(data) == packet["bytes"] == struct.unpack_from("<I", frame, native_offset)[0]
        native_offset += 4
        assert sha256(data) == packet["sha256"] and frame[native_offset:native_offset + len(data)] == data
        native_offset += len(data)
    assert native_offset == len(frame)
    result = subprocess.run([str(args.decoder.resolve()), str(fixture / "frame.pwvf"), str(width), str(height)], capture_output=True, text=True, check=True, timeout=30)
    decoded = json.loads(result.stdout.strip().splitlines()[-1])
    assert decoded["decoded_plane_sha256"] == manifest["decoded_plane_sha256"]
    print(f"PASS {width}x{height} {pattern}: source/native/PWVF SHA-256 and offline reference decode match")


if __name__ == "__main__":
    main()
