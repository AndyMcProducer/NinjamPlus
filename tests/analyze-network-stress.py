"""Compare independently measured audio and displayed timestamps across a fault run.

First generate full-run audio results with analyze-live-sync.py --window-seconds
900 --step-seconds 2, once per source. Missing video is reported separately from
valid audiovisual offsets; a frozen picture is not counted as healthy playback.
"""
import argparse
import bisect
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'test-results')
root = parser.parse_args().root
events = [json.loads(line) for line in (root / 'clumsy-events.jsonl').read_text(encoding='utf-8-sig').splitlines()]

def distribution(values):
    return {'count': len(values), 'median': round(statistics.median(values), 2),
            'min': round(min(values), 2), 'max': round(max(values), 2)} if values else {'count': 0}

report = {}
for source, receiver in [('alpha', 'bravo'), ('bravo', 'alpha')]:
    audio = json.loads((root / f'live-audio-analysis-{source}-to-{receiver}.json').read_text())
    browser = json.loads((root / f'clumsy-{receiver}.json').read_text())
    video = browser['samples']
    times = [sample['now'] for sample in video]
    comparisons = []
    for sample in audio:
        row = dict(sample)
        if sample.get('correlation', 0) >= 0.5 and video:
            index = bisect.bisect_left(times, sample['at'])
            candidates = [i for i in (index - 1, index) if 0 <= i < len(video)]
            index = min(candidates, key=lambda i: abs(times[i] - sample['at']))
            frame = video[index]
            if abs(frame['now'] - sample['at']) <= 300:
                row['videoDelayMs'] = frame['delay']
                row['videoMinusAudioMs'] = round(frame['delay'] - sample['audioDelayMs'], 2)
                prior = video[max(0, index - 1)]
                row['advancing'] = abs((frame['now'] - frame['delay']) - (prior['now'] - prior['delay'])) > 5
        comparisons.append(row)
    rows = []
    for start in (e for e in events if e['state'] == 'launch'):
        end = next((e['at'] for e in events if e['name'] == start['name'] and e['state'] == 'exited' and e['at'] > start['at']), None)
        recovery = next((e['at'] for e in events if e['name'] == start['name'] and e['state'] == 'recovery-end' and e['at'] > start['at']), None)
        if end is None:
            continue
        periods = [('fault', start['at'], end)]
        if recovery is not None and recovery - end > 10000:
            periods.append(('recoveryLast10s', recovery - 10000, recovery))
        for phase, first, last in periods:
            points = [p for p in comparisons if first <= p['at'] < last]
            offsets = [p['videoMinusAudioMs'] for p in points if 'videoMinusAudioMs' in p]
            moving = [p['videoMinusAudioMs'] for p in points if p.get('advancing')]
            network = [p for r in browser.get('network', []) if first <= r['at'] < last for p in r['peers']]
            rows.append({'case': start['name'], 'phase': phase,
                         'audioWindows': len(points), 'silentWindows': sum(bool(p.get('silent')) for p in points),
                         'videoMinusAudioMs': distribution(offsets), 'advancingVideoMinusAudioMs': distribution(moving),
                         'rttSeconds': distribution([p['rtt'] for p in network if 'rtt' in p]),
                         'relayProtocols': sorted(set(p['relayProtocol'] for p in network if p.get('relayProtocol')))})
    gaps = [b['now'] - a['now'] for a, b in zip(video, video[1:])]
    report[f'{source}-to-{receiver}'] = {'cases': rows, 'maxMissingTimestampGapMs': max(gaps, default=0), 'comparisons': comparisons}
(root / 'network-stress-summary.json').write_text(json.dumps(report, indent=2))
print(json.dumps({direction: {k: v for k, v in data.items() if k != 'comparisons'} for direction, data in report.items()}, indent=2))
