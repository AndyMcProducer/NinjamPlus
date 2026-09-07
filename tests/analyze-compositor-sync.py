"""Decode a known remote test-camera rectangle from actual Chrome screenshots.

Supply the visibly verified video rectangle in screenshot pixels, excluding any
letterboxing. Capture-call before/after timestamps bound screenshot timing; this
does not measure monitor scanout. Reject images whose barcode header is invalid.
"""
import argparse
import json
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('metadata', type=Path)
parser.add_argument('--left', type=float, required=True)
parser.add_argument('--top', type=float, required=True)
parser.add_argument('--width', type=float, required=True)
args = parser.parse_args()
records = json.loads(args.metadata.read_text())
if isinstance(records, dict):
    records = [dict(records, file=args.metadata.with_suffix('.png').name)]
results = []
for record in records:
    image = Image.open(args.metadata.parent / record['file']).convert('RGB')
    row_y = round(args.top + 50 * args.width / 1280)
    positions = [(round(args.left + (bit + .5) * args.width / 40), row_y) for bit in range(40)]
    if not all(0 <= x < image.width and 0 <= y < image.height for x, y in positions):
        raise SystemExit(f"Video rectangle is outside {record['file']}")
    bits = ''.join('1' if image.getpixel(point)[0] > 128 else '0' for point in positions)
    if int(bits[:8], 2) != 0xa5:
        raise SystemExit(f"Invalid barcode header in {record['file']}; verify the actual video rectangle")
    capture = int(bits[8:], 2)
    results.append({'file': record['file'], 'captureAgeLowerMs': (record['before'] - capture) & 0xffffffff,
                    'captureAgeUpperMs': (record['after'] - capture) & 0xffffffff})
print(json.dumps(results, indent=2))
