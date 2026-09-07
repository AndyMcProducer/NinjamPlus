"""Read synthetic capture clocks from actual Chrome compositor screenshots.

Rectangles are measured on the page in CSS pixels; --viewport-width supplies
the known screenshot viewport width so device scaling is accounted for.
"""
import argparse
import json
from pathlib import Path
from PIL import Image

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('metadata', type=Path)
p.add_argument('--viewport-width', type=int, default=1600)
a = p.parse_args()
results = []
for item in json.loads(a.metadata.read_text()):
    im = Image.open(a.metadata.parent / item['file']).convert('RGB')
    scale = im.width / a.viewport_width
    rect = item['rect']
    left, top, width = rect['x'] * scale, rect['y'] * scale, rect['width'] * scale
    y = round(top + width * 50 / 1280)
    bits = ''.join('1' if im.getpixel((round(left + (i + .5) * width / 40), y))[0] > 128 else '0' for i in range(40))
    valid = int(bits[:8], 2) == 0xa5
    capture = int(bits[8:], 2)
    results.append({'file': item['file'], 'valid': valid,
                    'ageLowerMs': (item['before'] - capture) & 0xffffffff if valid else None,
                    'ageUpperMs': (item['after'] - capture) & 0xffffffff if valid else None})
print(json.dumps(results, indent=2))
