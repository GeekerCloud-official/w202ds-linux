#!/usr/bin/env python3
"""MP4/H264 runtime cases and independent software-decoded frame digests."""
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import sys

import av
import numpy as np

base = Path(sys.argv[1])
base.mkdir(parents=True, exist_ok=False)
for name, width, height, count, profile, bframes in (
    ('long', 640, 360, 300, 'main', 2),
    ('crop', 854, 478, 90, 'high', 3),
    ('fullhd', 1920, 1080, 60, 'high', 2),
):
    path = base / f'{name}.mp4'
    container = av.open(path, mode='w')
    encoder = container.add_stream('libx264', rate=25)
    encoder.width, encoder.height, encoder.pix_fmt = width, height, 'yuv420p'
    encoder.options = {'preset': 'fast', 'crf': '22', 'profile': profile,
                       'x264-params': f'bframes={bframes}:keyint=50:scenecut=0'}
    yy, xx = np.arange(height)[:, None], np.arange(width)[None, :]
    cy, cx = np.arange(height // 2)[:, None], np.arange(width // 2)[None, :]
    for index in range(count):
        y = (16 + (xx * 3 + yy * 2 + index * 31) % 220).astype(np.uint8)
        u = (48 + (cx * 4 + cy + index * 19) % 160).astype(np.uint8)
        v = (48 + (cy * 3 + cx + index * 23) % 160).astype(np.uint8)
        array = np.concatenate((y.ravel(), u.ravel(), v.ravel())).reshape(height * 3 // 2, width)
        frame = av.VideoFrame.from_ndarray(array, format='yuv420p')
        frame.pts, frame.time_base = index, Fraction(1, 25)
        for packet in encoder.encode(frame):
            container.mux(packet)
    for packet in encoder.encode(None):
        container.mux(packet)
    container.close()
    digest = hashlib.sha256()
    frames = []
    byte_count = 0
    with av.open(path) as reference:
        assert reference.streams.video[0].codec_context.name == 'h264'
        for frame in reference.decode(video=0):
            raw = frame.to_ndarray(format='yuv420p').tobytes()
            assert frame.width == width and frame.height == height
            assert len(raw) == width * height * 3 // 2
            digest.update(raw)
            byte_count += len(raw)
            frames.append(hashlib.sha256(raw).hexdigest())
    assert len(frames) == count
    report = {'name': name, 'width': width, 'height': height, 'frames': count,
              'profile': profile, 'bframes': bframes, 'fps': [25, 1],
              'input_bytes': path.stat().st_size,
              'input_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'reference_format': 'yuv420p', 'reference_bytes': byte_count,
              'reference_sha256': digest.hexdigest(), 'reference_frame_sha256': frames,
              'pyav': av.__version__, 'numpy': np.__version__,
              'ffmpeg': {k: list(v) for k, v in av.library_versions.items()},
              'reference': 'Independent FFmpeg software H264 decoder, no hardware context',
              'source': 'Generated deterministic colour/motion patterns'}
    (base / f'{name}.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('name', 'input_bytes', 'frames', 'reference_sha256')}), flush=True)
