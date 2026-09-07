"""Compare native waveform delay with independently timestamped displayed video.

Run analyze-live-sync.py for both directions first. Browser snapshots may span
helper reloads; each generation is saved separately by run-native-vdo-browser.
"""
import argparse
import bisect
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--from-ms', type=int, default=0)
parser.add_argument('--to-ms', type=int, default=2**63-1)
args = parser.parse_args()

def nearest(rows, times, timestamp):
    index = bisect.bisect_left(times, timestamp)
    candidates = [i for i in (index-1, index) if 0 <= i < len(rows)]
    return min(candidates, key=lambda i: abs(times[i]-timestamp)) if candidates else None

report = {}
for receiver, source in [('alpha', 'bravo'), ('bravo', 'alpha')]:
    files = [json.loads(p.read_text()) for p in args.root.glob(receiver+'-[0-9]*.json')]
    video = sorted([s for f in files for s in f['samples']], key=lambda s:s['now'])
    source_files = [json.loads(p.read_text()) for p in args.root.glob(source+'-[0-9]*.json')]
    preview = sorted([s for f in source_files for s in f['source']], key=lambda s:s['at'])
    vt, pt = [s['now'] for s in video], [s['at'] for s in preview]
    audio = json.loads((args.root/f'live-audio-analysis-{source}-to-{receiver}.json').read_text())
    comparisons = []
    for a in audio:
        if not args.from_ms <= a['at'] <= args.to_ms:
            continue
        row = dict(a)
        i = nearest(video, vt, a['at'])
        if a.get('correlation', 0) >= .5 and i is not None and abs(vt[i]-a['at']) <= 300:
            v = video[i]
            capture = v['now']-v['delay']
            prior = video[max(0, i-1)]
            row.update(videoDelayMs=v['delay'], videoMinusAudioMs=round(v['delay']-a['audioDelayMs'], 2),
                       advancing=capture-(prior['now']-prior['delay']) > 5)
            j = nearest(preview, pt, a['at'])
            row['sourceFresh'] = j is not None and abs(pt[j]-a['at']) < 1000 and 0 <= preview[j].get('captureAgeMs', -1) < 500
        comparisons.append(row)
    observed = [r['videoMinusAudioMs'] for r in comparisons if r.get('advancing') and r.get('sourceFresh')]
    report[source+'-to-'+receiver] = {'audioWindows': len(comparisons), 'advancingFreshSourceWindows':len(observed),
        'medianVideoMinusAudioMs':round(statistics.median(observed),2) if observed else None,
        'maxAbsVideoMinusAudioMs':round(max(map(abs,observed)),2) if observed else None,
        'rows':comparisons}
print(json.dumps(report, indent=2))
