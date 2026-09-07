"""Summarize this fixed-build TCP/UDP run against independent waveform matches."""
import argparse, bisect, json, statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'test-results/playout-stress-fixed')
root = parser.parse_args().root
def stats(values):
    return {'n':len(values), 'median':round(statistics.median(values),2), 'min':round(min(values),2), 'max':round(max(values),2)} if values else None
result = {}
for protocol, prefix in [('tcp','tcp-final'),('udp','udp-network-final')]:
    events = [json.loads(line) for line in (root/f'{protocol}-clumsy-events.jsonl').read_text(encoding='utf-8-sig').splitlines()]
    start = max(e['at'] for e in events if e['name']=='suite' and e['state']=='start' and e['details'].get('protocol')==protocol)
    events = [e for e in events if e['at']>=start]
    windows = [(e['name'],e['at']-10000,e['at']) for e in events if e['state']=='recovery-end']
    result[protocol] = {}
    for index, source, receiver in [(0,'bravo','alpha'),(1,'alpha','bravo')]:
        d = json.loads((root/f'{prefix}-{index}.json').read_text())
        audio = [r for r in json.loads((root/f'live-audio-analysis-{source}-to-{receiver}.json').read_text()) if r.get('correlation',0)>=.5]
        times = [r['at'] for r in audio]
        rows = []
        previous = {}
        for r in d['samples']:
            capture = r['now']-r['delay']
            advancing = r['id'] not in previous or capture-previous[r['id']]>5
            previous[r['id']] = capture
            if not advancing or not times: continue
            i=bisect.bisect_left(times,r['now'])
            near=min([j for j in [i-1,i] if 0<=j<len(times)],key=lambda j:abs(times[j]-r['now']))
            if abs(times[near]-r['now'])<=2600:
                rows.append({'at':r['now'],'video':r['delay'],'audio':audio[near]['audioDelayMs'],'error':r['delay']-audio[near]['audioDelayMs']})
        summary = {}
        for name,lo,hi in windows:
            selected=[r for r in rows if lo<=r['at']<=hi]
            summary[name]={k:stats([r[k] for r in selected]) for k in ['video','audio','error']}
        rtt=[p['rtt'] for r in d['network'] for p in r['peers'] if p.get('rtt') is not None]
        result[protocol][receiver]={'recovery':summary,'maxRttSeconds':max(rtt) if rtt else None,
                                  'lastRemotePixelAt':d['samples'][-1]['now'] if d['samples'] else None,
                                  'lastNetworkAt':d['network'][-1]['at'], 'lastPeerCount':len(d['network'][-1]['peers'])}
(root/'run-summary.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
