"""Measure writer-to-presentation timing without assuming stable media timestamps."""
import argparse
import json
import statistics
from pathlib import Path


def summary(values):
    values = sorted(values)
    if not values:
        return None
    return {'n': len(values), 'median': round(statistics.median(values), 2),
            'p95': round(values[int(.95 * (len(values) - 1))], 2),
            'min': round(values[0], 2), 'max': round(values[-1], 2)}


def analyze(data, start=0, end=2**63-1):
    decoded = {}
    for frame in data['observer']['decoded']:
        identity = frame.get('frameIdentity')
        if identity is not None:
            decoded.setdefault((frame['peer'], identity), []).append(frame)
    wall, browser, gaps, rebases = [], [], [], []
    previous = {}
    count = ambiguous = 0
    for frame in data['observer']['presented']:
        if not start <= frame['at'] <= end:
            continue
        count += 1
        if frame['peer'] in previous:
            gaps.append(frame['at'] - previous[frame['peer']])
        previous[frame['peer']] = frame['at']
        identity = frame.get('frameIdentity')
        candidates = [f for f in decoded.get((frame['peer'], identity), [])
                      if 0 <= frame['at'] - f['at'] < 60000]
        # Repeated content submitted more than once cannot identify which write
        # produced the displayed frame. Do not silently choose the latest write.
        if len(candidates) != 1:
            ambiguous += len(candidates) > 1
            continue
        source = candidates[0]
        wall.append(frame['at'] - source['at'])
        browser.append(frame['expectedDisplayTime'] - source['perf'])
        rebases.append(frame['mediaTime'] * 1000 - source['timestamp'] / 1000)
    pixels = [r['delay'] for r in data.get('samples', []) if start <= r['now'] <= end]
    return {'presentedFrames': count, 'matchedFrames': len(wall),
            'unmatchedFrames': count - len(wall), 'ambiguousFrames': ambiguous,
            'matchingComplete': count > 0 and len(wall) == count,
            'matchingMethod': 'independent pixel barcode',
            'warning': 'Unmatched or repeated content cannot establish writer-to-presentation delay.' if len(wall) != count else None,
            'writerToCallbackWallMs': summary(wall),
            'writerToExpectedDisplayMs': summary(browser),
            'presentationMinusWriterTimestampMs': summary(rebases),
            'callbackGapsMs': summary(gaps), 'videoPixelAgeMs': summary(pixels)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--from-ms', type=int, default=0)
    parser.add_argument('--to-ms', type=int, default=2**63-1)
    args = parser.parse_args()
    print(json.dumps(analyze(json.loads(args.snapshot.read_text()), args.from_ms, args.to_ms), indent=2))
