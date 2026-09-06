"""Compare generated source audio with real NINJAM decoded output (48 kHz float32)."""
from pathlib import Path
import json
import numpy as np
from scipy.signal import correlate

root = Path(__file__).resolve().parents[1] / "test-results"
source = np.memmap(root / "live-alpha-input.f32", dtype="float32", mode="r")
received = np.memmap(root / "live-bravo-output.f32", dtype="float32", mode="r")
source_clock = np.loadtxt(root / "live-alpha-clock.txt", dtype="int64")
received_clock = np.loadtxt(root / "live-bravo-clock.txt", dtype="int64")
last_block = min(len(received) // 480, len(received_clock)) - 200
results = []
for block in range(max(0, last_block - 2000), last_block, 200):
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
(root / "live-audio-analysis.json").write_text(json.dumps(results, indent=2))
matched = [entry for entry in results if entry.get("correlation", 0) >= 0.5]
if len(matched) < 5:
    raise SystemExit("FAIL: insufficient correlated audio windows; delay measurement is not validated")
