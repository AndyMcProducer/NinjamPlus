"""Summarize displayed timestamp ages per bounded Clumsy case and recovery."""
from pathlib import Path
import json
import statistics
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'test-results')
root = parser.parse_args().root
events = [json.loads(line) for line in (root / 'clumsy-events.jsonl').read_text(encoding='utf-8-sig').splitlines()]
report = {}
for receiver in ('alpha', 'bravo'):
    samples = json.loads((root / f'clumsy-{receiver}.json').read_text())['samples']
    rows = []
    for event in events:
        if event['state'] != 'launch':
            continue
        end = next((e['at'] for e in events if e['name'] == event['name'] and e['state'] == 'exited' and e['at'] > event['at']), None)
        if end is None:
            continue
        row = {'case': event['name'], 'start': event['at'], 'end': end}
        recovery_end = next((e['at'] for e in events if e['name'] == event['name'] and e['state'] == 'recovery-end' and e['at'] >= end), None)
        windows = [('fault', event['at'], end)]
        if recovery_end is not None and recovery_end - end >= 10000:
            windows.append(('recoveredLast10s', recovery_end - 10000, recovery_end))
        for name, start, finish in windows:
            values = [s['delay'] for s in samples if start <= s['now'] < finish]
            row[name] = {'count': len(values), 'medianMs': statistics.median(values), 'maxMs': max(values), 'minMs': min(values)} if values else None
        rows.append(row)
    report[receiver] = rows
(root / 'clumsy-summary.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
