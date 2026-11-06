#!/usr/bin/env python3
"""Deterministic real H264 and independent FFmpeg pixel references (PyAV)."""
from fractions import Fraction
import argparse
import hashlib
import json
from pathlib import Path

import av
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('target', type=Path)
parser.add_argument('--width', type=int, default=64)
parser.add_argument('--height', type=int, default=64)
parser.add_argument('--frames', type=int, default=4)
parser.add_argument('--gop', type=int, default=1)
parser.add_argument('--bframes', type=int, default=0)
args = parser.parse_args()
assert 16 <= args.width <= 512 and args.width % 16 == 0
assert 16 <= args.height <= 512 and args.height % 16 == 0
assert 1 <= args.frames <= 12 and 1 <= args.gop <= 12 and 0 <= args.bframes <= 2
target = args.target
target.mkdir(parents=True, exist_ok=False)
width, height, count = args.width, args.height, args.frames
codec = av.CodecContext.create("libx264", "w")
codec.width, codec.height = width, height
codec.pix_fmt = "yuv420p"
codec.time_base = Fraction(1, 25)
codec.framerate = Fraction(25, 1)
profile = 'main' if args.bframes else 'baseline'
codec.options = {"preset": "ultrafast", "tune": "zerolatency", "profile": profile,
                 "x264-params": f"bframes={args.bframes}:keyint={args.gop}:repeat-headers=1:annexb=1", "crf": "18"}
packets = []
for index in range(count):
    yy, xx = np.indices((height, width))
    image = np.empty((height * 3 // 2, width), dtype=np.uint8)
    image[:height] = (16 + (xx * 3 + yy * 2 + index * 31) % 220).astype(np.uint8)
    chroma_y, chroma_x = np.indices((height // 2, width // 2))
    image[height:height + height // 4] = (
        48 + (chroma_x * 4 + chroma_y + index * 19) % 160).astype(np.uint8).reshape(height // 4, width)
    image[height + height // 4:] = (
        48 + (chroma_y * 3 + chroma_x + index * 23) % 160).astype(np.uint8).reshape(height // 4, width)
    frame = av.VideoFrame.from_ndarray(image, format="yuv420p")
    frame.pts = index
    packets.extend(bytes(packet) for packet in codec.encode(frame))
packets.extend(bytes(packet) for packet in codec.encode(None))
stream = b"".join(packets)
(target / "baseline.h264").write_bytes(stream)
decoder = av.CodecContext.create("h264", "r")
frames = []
for packet in decoder.parse(stream):
    frames.extend(decoder.decode(packet))
for packet in decoder.parse(b""):
    frames.extend(decoder.decode(packet))
frames.extend(decoder.decode(None))
assert len(frames) == count
records = {}
for fmt in ("nv12", "nv21", "yuv420p"):
    output = []
    for frame in frames:
        if fmt == "nv21":
            image = frame.to_ndarray(format="nv12").copy()
            image[height:] = image[height:].reshape(-1, 2)[:, ::-1].reshape(height // 2, width)
        else:
            image = frame.to_ndarray(format=fmt)
        output.append(image.tobytes())
    raw = b"".join(output)
    assert len(raw) == width * height * 3 // 2 * count
    path = target / f"reference.{fmt}"
    path.write_bytes(raw)
    records[path.name] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
records["baseline.h264"] = {"bytes": len(stream), "sha256": hashlib.sha256(stream).hexdigest()}
report = {"width": width, "height": height, "frames": count,
          "profile": f"H264 {profile}", "gop": args.gop, "bframes": args.bframes,
          "source": "generated color/motion pattern, no external media", "pyav": av.__version__,
          "ffmpeg": {name: list(version) for name, version in av.library_versions.items()},
          "packet_lengths": [len(packet) for packet in packets], "files": records}
(target / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
