"""Generate a loopable native 1080p60 Y4M source for Chrome's fake camera."""
from pathlib import Path
import argparse
import numpy as np
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
x, y = np.arange(1920)[None, :], np.arange(1080)[:, None]
with args.output.open('wb') as target:
    target.write(b'YUV4MPEG2 W1920 H1080 F60:1 Ip A1:1 C420jpeg\n')
    for frame in range(120):
        luma = np.asarray(32 + ((x // 16 + y // 16 + frame // 2) % 2) * 100
                          + ((x + frame * 12) % 1920 < 160) * 90, dtype=np.uint8)
        target.write(b'FRAME\n')
        target.write(luma.tobytes())
        target.write(bytes([100 + frame % 40]) * (960 * 540))
        target.write(bytes([160 - frame % 40]) * (960 * 540))
print(args.output)
