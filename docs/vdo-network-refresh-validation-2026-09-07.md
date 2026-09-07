# Refresh fix: network recovery validation

Result: the zero-delay refresh regression stayed fixed, but the full network
recovery test did not pass. Do not treat this build as validated for Bernd's
reported persistent one-interval video lead.

Used the rebuilt native harness and embedded helper, installed Chrome, deployed
VDO alpha (webrtc 962), two generated camera sources and independently recorded
native audio waveforms. No receiver source override. Native hotspot mode was
enabled; the selected media route was TURN/TLS over TCP to 51.222.12.223:443.
BPM 190 / BPI 16 gives a 5052.63 ms interval. This run did not test UDP.

Clumsy applied the following configured faults, with 45 seconds of recovery
after each. Percentages describe configured packet dropping, not measured
application-level loss after TCP retransmission.

- 20% drop, 300 ms lag, 500 ms jitter, approximately 25 seconds.
- 40% drop, 800 ms lag, 800 ms jitter, approximately 20 seconds.
- 100% drop for 12 seconds, targeting both the selected TURN connection and
  the private native NINJAM server connection.

The suite completed at epoch 1788761240053 ms. Clumsy and both test clients were
stopped after capture; waveform analysis ran afterward. Artifacts are under
`test-results/refresh-fix-network-tcp`. The filter targets the selected relay;
this does not prove that every possible alternate network route was impaired.

Independent waveform/pixel comparison (video minus audio; positive means late):

| Window | Bravo to alpha | Alpha to bravo |
| --- | --- | --- |
| Baseline | 9/9 advancing, median +59 ms, max absolute 189 ms | 9/9 advancing, median +27 ms, max absolute 127 ms |
| Last 15 seconds after 20% case | 3/4 advancing, max absolute 817 ms | 4/4 advancing, max absolute 825 ms |
| Last 15 seconds after 40% case | 3/4 advancing, max absolute 5323 ms | 3/4 advancing, max absolute 870 ms |
| Last 15 seconds after outage | 4/4 advancing, max absolute 229 ms | 3/4 advancing, max absolute 106 ms |
| Additional clean observation | 11/25 advancing | 13/25 advancing |

Maxima in the table use advancing samples only. Frozen and missing samples
remain in the denominators and prevent a pass, regardless of a small median.
The additional clean window was epoch 1788761260000 through 1788761370000 ms.
It showed repeated freezes in both directions; sampled frozen frames became
about 5.7 seconds later than audio. Native audio remained around 5.05 seconds
old. This is not evidence of a persistent one-interval video lead.

Receiver message traces recorded zero instances of a zero buffer command after
a positive target, and no out-of-order enqueue events. Thus the helper refresh
fix held during this run. However, playout drops accumulated to 1954/1889.
AudioContext oscillator callbacks in both browser clients were delayed at the
same times, reaching approximately 1.93 seconds beyond their scheduled times.
Recorded long tasks did not explain delays of that size. This identifies a
timing symptom, not its cause: browser scheduling, shared host behavior, and
instrumentation effects still need to be separated from network recovery.

Next isolation test: repeat the same duration with no Clumsy faults, record
visibility and AudioContext state transitions, and compare a lightweight pixel
capture against the full timing instrumentation. Then repeat UDP impairment
with a verified UDP filter. Do not change production scheduling solely on this
run, or publish the package as having passed network recovery.

The portable package is staged locally, but no new release or push was made
under the conditional network-test pass criterion.
