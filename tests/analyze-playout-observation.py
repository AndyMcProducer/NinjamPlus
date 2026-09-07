"""Compare actual presented VDO frames to the native requested delay, without claps.

Input: live-playout-observer.js export JSON and timestamped /intervals JSONL.
This checks receiver playout, not the correctness of the sender capture clock
or physical camera/audio-device latency.
"""
import argparse
import bisect
import json
from pathlib import Path
import statistics


def summarize(values):
    if not values:
        return None
    values = sorted(values)
    return {"count": len(values), "median": round(statistics.median(values), 2),
            "p95": round(values[int((len(values)-1)*.95)], 2), "max": round(values[-1], 2),
            "min": round(values[0], 2)}


def analyze(observation, native_rows):
    native = []
    for row in native_rows:
        entries = row.get("intervals", [])
        if isinstance(entries, dict):
            entries = entries.get("value", [])  # PowerShell array wrapper
        info = next((entry for entry in entries if entry.get("type") == "intervalInfo"), {})
        for entry in entries:
            if entry.get("type") == "videoTimecode" and "bufferTotalMs" in entry:
                native.append({"at": info.get("wallClockMs", row["at"]), "label": entry.get("userKey"),
                               "target": entry["bufferTotalMs"],
                               "intervalMs": 60000 * info.get("bpi", 0) / max(1, info.get("bpm", 1))})
    peers = {}
    for snapshot in observation["snapshots"]:
        peers[snapshot["peer"]] = snapshot.get("label")
    results = {}
    decoded = {}
    for frame in observation["decoded"]:
        decoded.setdefault((frame["peer"], round(frame["timestamp"])), []).append(frame)
    for candidates in decoded.values():
        candidates.sort(key=lambda frame: frame["at"])
    for peer, label in peers.items():
        targets = sorted((entry for entry in native if entry["label"] == label), key=lambda entry: entry["at"])
        target_times = [entry["at"] for entry in targets]
        frames = [frame for frame in observation["presented"] if frame["peer"] == peer]
        gaps, ages, held_ages, display_delays, errors = [], [], [], [], []
        matched = 0
        prior_age = None
        for index, frame in enumerate(frames):
            if index:
                gap = frame["expectedDisplayTime"] - frames[index-1]["expectedDisplayTime"]
                gaps.append(gap)
                if prior_age is not None:
                    held_ages.append(prior_age + max(0, gap))
            candidates = decoded.get((peer, round(frame["mediaTime"] * 1e6)), [])
            # The same media timestamp can recur after a sender restarts. Do not
            # match an old presentation to the later incarnation of that frame.
            candidate_index = bisect.bisect_right([row["at"] for row in candidates], frame["at"] + 50) - 1
            if candidate_index < 0:
                prior_age = None
                continue
            source = candidates[candidate_index]
            matched += 1
            display_delay = frame["expectedDisplayTime"] - source["perf"]
            display_delays.append(display_delay)
            age = source["estimatedCaptureAgeMs"] + display_delay
            prior_age = age
            ages.append(age)
            pos = bisect.bisect_right(target_times, frame["at"]) - 1
            if pos >= 0 and 0 <= frame["at"] - targets[pos]["at"] <= 2000:
                errors.append(age - targets[pos]["target"])
        results[label or peer] = {"presentedFrames": len(frames), "matchedDecodedFrames": matched,
            "displayedFrameAgeMs": summarize(ages), "decoderOutputToDisplayMs": summarize(display_delays),
            "heldFrameAgeBeforeNextPresentationMs": summarize(held_ages),
            "displayGapMs": summarize(gaps), "displayAgeMinusNativeTargetMs": summarize(errors),
            "nativeTargetMs": summarize([entry["target"] for entry in targets]),
            "interpretation": "Positive deviation means older video than the native requested delay; this is not an independent audio/camera capture calibration."}
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observation", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("--from-ms", type=int, default=0)
    parser.add_argument("--to-ms", type=int, default=2**63-1)
    args = parser.parse_args()
    observation = json.loads(args.observation.read_text(encoding="utf-8-sig"))
    # Preserve decode records for displayed frames near the selected start.
    observation["presented"] = [row for row in observation["presented"] if args.from_ms <= row["at"] <= args.to_ms]
    native = [json.loads(line) for line in args.native.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    print(json.dumps(analyze(observation, native), indent=2))
