#!/usr/bin/env python3
"""Deterministic real H264 and independent FFmpeg pixel references (PyAV)."""
from fractions import Fraction
import argparse
import hashlib
import json
from pathlib import Path
import re

import av
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('target', type=Path)
parser.add_argument('--width', type=int, default=64)
parser.add_argument('--height', type=int, default=64)
parser.add_argument('--frames', type=int, default=4)
parser.add_argument('--gop', type=int, default=1)
parser.add_argument('--bframes', type=int, default=0)
parser.add_argument('--profile', choices=('baseline', 'main', 'high'))
parser.add_argument('--cabac', type=int, choices=(0, 1))
parser.add_argument('--transform8x8', type=int, choices=(0, 1), default=0)
parser.add_argument('--cqm', choices=('flat', 'jvt', 'asymmetric'), default='flat')
parser.add_argument('--chroma-qp-offset', type=int, default=0)
args = parser.parse_args()
assert 16 <= args.width <= 1920 and args.width % 2 == 0
assert 16 <= args.height <= 1088 and args.height % 2 == 0
assert 1 <= args.frames <= 32 and 1 <= args.gop <= 1000 and 0 <= args.bframes <= 2
assert -12 <= args.chroma_qp_offset <= 12
target = args.target
target.mkdir(parents=True, exist_ok=False)
width, height, count = args.width, args.height, args.frames
codec = av.CodecContext.create("libx264", "w")
codec.width, codec.height = width, height
codec.pix_fmt = "yuv420p"
codec.time_base = Fraction(1, 25)
codec.framerate = Fraction(25, 1)
profile = args.profile or ('main' if args.bframes else 'baseline')
cabac = args.cabac if args.cabac is not None else int(profile != 'baseline')
assert profile != 'baseline' or not (args.bframes or cabac)
assert profile == 'high' or not (args.transform8x8 or args.cqm != 'flat')
params = (f"bframes={args.bframes}:keyint={args.gop}:min-keyint={args.gop}:scenecut=0:"
          f"repeat-headers=1:annexb=1:ref=1:weightp=0:weightb=0:b-pyramid=none:"
          f"direct=spatial:b-adapt=0:threads=1:sliced-threads=0:slices=1:cabac={cabac}:8x8dct={args.transform8x8}:"
          f"chroma-qp-offset={args.chroma_qp_offset}")
matrices = {}
if args.transform8x8:
    params += ':analyse=i4x4,i8x8,p8x8,b8x8:subme=6'
if args.cqm == 'jvt':
    params += ':cqm=jvt'
elif args.cqm == 'asymmetric':
    # Distinct, non-symmetric raster matrices reveal transpose/scan mistakes.
    for index, name in enumerate(('cqm4iy', 'cqm4ic', 'cqm4py', 'cqm4pc', 'cqm8i', 'cqm8p')):
        side = 8 if name.startswith('cqm8') else 4
        values = [8 + (row * 7 + col * 3 + index * 5) % 40
                  for row in range(side) for col in range(side)]
        matrices[name] = values
        params += ':' + name + '=' + ','.join(map(str, values))
codec.options = {"preset": "ultrafast", "tune": "zerolatency", "profile": profile,
                 "x264-params": params, "crf": "18"}
av.logging.set_level(av.logging.INFO)
with av.logging.Capture(local=False) as opening_logs:
    codec.open()
assert not [entry for entry in opening_logs if entry[0] <= av.logging.WARNING], opening_logs
packets = []
for index in range(count):
    yy, xx = np.indices((height, width))
    y = (16 + (xx * 3 + yy * 2 + index * 31) % 220).astype(np.uint8)
    chroma_y, chroma_x = np.indices((height // 2, width // 2))
    u = (48 + (chroma_x * 4 + chroma_y + index * 19) % 160).astype(np.uint8)
    v = (48 + (chroma_y * 3 + chroma_x + index * 23) % 160).astype(np.uint8)
    image = np.frombuffer(y.tobytes() + u.tobytes() + v.tobytes(), dtype=np.uint8).reshape(height * 3 // 2, width)
    frame = av.VideoFrame.from_ndarray(image, format="yuv420p")
    frame.pts = index
    packets.extend(bytes(packet) for packet in codec.encode(frame))
packets.extend(bytes(packet) for packet in codec.encode(None))
with av.logging.Capture(local=False) as closing_logs:
    del codec
encoder_log = ''.join(entry[2] for entry in opening_logs + closing_logs)
(target / 'encoder.log').write_text(encoder_log)
stream = b"".join(packets)
nal_units = [nal for nal in re.split(b'\x00\x00\x00?\x01', stream) if nal]
sps_profile = next(nal[1] for nal in nal_units if nal[0] & 31 == 7)
# Zerolatency's sliced threads can override a requested slice count. Keep this
# diagnostic corpus genuinely single-slice, including at 1080p.
assert sum(nal[0] & 31 in (1, 5) for nal in nal_units) == count
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
          "profile": f"H264 { {66: 'baseline', 77: 'main', 100: 'high'}[sps_profile]}",
          "requested_profile": profile, "sps_profile_idc": sps_profile,
          "gop": args.gop, "bframes": args.bframes,
          "cabac": cabac, "transform8x8": args.transform8x8, "cqm": args.cqm,
          "custom_raster_matrices": matrices, "x264_params": params,
          "source": "generated color/motion pattern, no external media", "pyav": av.__version__,
          "ffmpeg": {name: list(version) for name, version in av.library_versions.items()},
          "packet_lengths": [len(packet) for packet in packets], "files": records}
(target / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
