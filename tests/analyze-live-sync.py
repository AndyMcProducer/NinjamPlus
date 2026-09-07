"""Compare generated source audio with real NINJAM decoded output (48 kHz float32)."""
from pathlib import Path
import json
import argparse
import numpy as np
from scipy.signal import correlate

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1] / "test-results")
parser.add_argument("--source", choices=["alpha", "bravo"], default="alpha")
parser.add_argument("--end-ms", type=int, help="End of the measurement window, as a Unix timestamp in milliseconds")
parser.add_argument("--window-seconds", type=int, default=20)
parser.add_argument("--step-seconds", type=int, default=2)
args = parser.parse_args()
root = args.root
if args.window_seconds < 1 or args.step_seconds < 1:
    parser.error("window and step must be positive")
receiver = "bravo" if args.source == "alpha" else "alpha"
source = np.memmap(root / f"live-{args.source}-input.f32", dtype="float32", mode="r")
received = np.memmap(root / f"live-{receiver}-output.f32", dtype="float32", mode="r")
source_clock = np.loadtxt(root / f"live-{args.source}-clock.txt", dtype="int64")
received_clock = np.loadtxt(root / f"live-{receiver}-clock.txt", dtype="int64")
last_block = min(len(received) // 480, len(received_clock)) - 200
if args.end_ms is not None:
    last_block = min(last_block, int(np.searchsorted(received_clock, args.end_ms)))
results = []
for block in range(max(0, last_block - args.window_seconds * 100), last_block, args.step_seconds * 100):
    timestamp = int(received_clock[block])
    window = np.asarray(received[block * 480:block * 480 + 4096], dtype="float64")
    first = int(np.searchsorted(source_clock, timestamp - 25000)) * 480
    last = min(len(source), int(np.searchsorted(source_clock, timestamp + 500)) * 480)
    candidates = np.asarray(source[first:last], dtype="float64")
    if len(candidates) < len(window) or np.max(np.abs(window)) < 1e-5:
        results.append({"at": timestamp, "silent": True})
        continue
    correlations = correlate(candidates, window, mode="valid", method="fft")
    peak = int(np.argmax(np.abs(correlations)))
    norm = np.linalg.norm(candidates[peak:peak + len(window)]) * np.linalg.norm(window)
    confidence = float(abs(correlations[peak]) / norm) if norm else 0
    matched_sample = first + peak
    source_time = float(source_clock[matched_sample // 480]) + (matched_sample % 480) / 48.0
    results.append({"at": timestamp, "audioDelayMs": round(timestamp - source_time, 2),
                    "correlation": round(confidence, 4)})
print(json.dumps(results, indent=2))
if args.source == "alpha":
    (root / "live-audio-analysis.json").write_text(json.dumps(results, indent=2))
(root / f"live-audio-analysis-{args.source}-to-{receiver}.json").write_text(json.dumps(results, indent=2))
matched = [entry for entry in results if entry.get("correlation", 0) >= 0.5]
if len(matched) < 5:
    raise SystemExit("FAIL: insufficient correlated audio windows; delay measurement is not validated")
