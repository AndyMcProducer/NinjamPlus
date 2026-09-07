# Video presentation and network stress on the fixed native build

This run uses two native synthetic-audio clients, the app's real helper pages,
and the installed Chrome executable in a dedicated debugging profile. Helper
URLs retain the app's codec, buffer, and relay settings; the UDP run removes only
`tcp`. The profile has no personal extensions or sessions. Both clients share
one Windows host and clock. No physical camera/audio device or monitor scanout
is measured.

The camera contains a nonrepeating capture timestamp in its pixels. Observations
include native GUID diagnostics, waveform captures, selected ICE relay transport,
writer submissions, video-frame callbacks, video-element pixel reads, and actual
Chrome screenshots. The screenshot decoder accounts for device scaling and
rejects an invalid barcode rather than treating an overlay as a valid frame.

## Final-writer overload

`tests/live-presentation-stress.js` sits downstream of the app decoder/scheduler.
The first clean TCP trial held eight seconds of decoded frames (bounded at 240
clones), then submitted them together without waiting for writer backpressure.
The writer reached 240 pending writes and desired size -238. All writes settled.
Matching displayed timestamps to writer submissions measured a median 5 ms and
maximum 46 ms from submission to callback delivery. The presentation gap reached
8026 ms while frames were held. Element pixels grew to 12475 ms old during the
freeze, but recovered near their original age after release.

Actual compositor screenshot capture-age bounds were 4387–4570 ms before the
burst and 4387–4584 ms after recovery. This distinguishes the deliberate upstream
hold from a persistent hidden presentation queue. The result is consistent with
Chrome discarding most burst frames; it does not establish that Chrome can never
retain frames under other timestamps or workloads.

An eight-second `video.pause()` trial overlapped the first TCP network fault.
It is a combined stress case, not an isolated pause result. The screenshots also
showed advancing video during the early part of that trial, so a single pause
call did not establish a sustained paused element in this application.

## Network conditions

Clumsy filters target only relay 51.222.12.223: TCP 443 or UDP 3478. Candidate
`relayProtocol` and TURN URL establish the real transport; candidate `protocol`
alone says UDP even with TCP/TLS TURN. TCP and UDP each receive:

- 20% configured loss, 300 ms lag, 500 ms random throttle holds for 25 seconds;
- 40% loss, 800 ms lag, 800 ms throttle holds for 20 seconds;
- 45 seconds recovery after each.

TCP also receives a 12-second total outage affecting both relay traffic and the
private local NINJAM TCP port, followed by 45 seconds recovery. UDP omits that
third case. Throttle holds are a bounded jitter approximation, not a geographic
network model or proof of the exact applied loss percentage.

After the TCP combined outage, both sources remained fresh (14–32 ms camera age)
but both receiver peer maps were empty at 1788749687650, over 80 seconds after
impairment ended. The last remote pixel samples were around 1788749595xxx. This
is a missing-peer/reconnect failure, not evidence of an internal video-element
queue. New pages were used for the UDP test.

## Artifacts and interpretation

Artifacts are under `test-results/playout-stress-fixed/`. The original native
stress captures and previous report remain separate. New analysis scripts are
`tests/analyze-presentation-screenshots.py` and
`tests/analyze-presentation-timing.py`. The latter reports wall-clock callback
delivery separately from `expectedDisplayTime`, avoiding an assumption that
unvalidated timestamp domains can simply be added to a buffer setting.

Chrome documents video-frame callbacks as best-effort compositor observations,
not physical display guarantees: [video-frame callback documentation](https://web.dev/articles/requestvideoframecallback-rvfc).
The dedicated profile follows Chrome's [remote debugging requirements](https://developer.chrome.com/blog/remote-debugging-port).

## Waveform validation and timestamp experiment

Offline audio correlation ran only after native capture and cameras stopped.
Both source directions passed the minimum correlation checks. With advancing
video in the last ten seconds of each recovery window, median video-minus-audio
offsets were:

| Condition | TCP at alpha / bravo | UDP at alpha / bravo |
| --- | --- | --- |
| 20% loss, 300 ms lag, 500 ms holds | +46 / +34 ms | +74 / +60 ms |
| 40% loss, 800 ms lag, 800 ms holds | +44 ms / no advancing frames | +83 / +63 ms |
| Combined outage | No remote video at either receiver | Not run |

Positive offsets mean older video. Audio matches are sampled every five seconds;
video is paired to the nearest match within 2600 ms. These are sampled stable
recovery measurements, not precise transition measurements or a guarantee about
every frame. Maximum observed relay RTT was 19.72 seconds for TCP and 5.311
seconds for UDP. TCP's second direction had already stopped advancing before the
combined outage; the final disconnected state cannot all be attributed to that
last outage alone. The screenshot after the outage still shows a green native
buffer indicator while the VDO area contains only the local preview.

The UDP player later had an additional upstream presentation interruption after
the formal recovery window. Its screenshot showed 6543–6852 ms capture age, while
the native audio remained about 5053 ms old. A planned eight-second future-PTS
test began during that interruption. No frame was submitted to the final writer
for the first 3427 ms of the test. The 9919–10153 ms-old screenshot during that
period therefore must not be attributed to Chrome queuing the altered timestamps.

Once submissions resumed, the test added 4000000 microseconds to each submitted
VideoFrame timestamp. Chrome's displayed `mediaTime` remained 4000000 microseconds
behind those submitted timestamps. During the actual altered-write window,
135 presentation callbacks had **zero exact timestamp matches** to submissions;
the displayed pixels nevertheless measured a median age of 5075 ms, close to
the independently recorded audio delay. Writer pending count never exceeded one.
After removing the injection, screenshots measured 4994–5129 ms and 4989–5243 ms.

This demonstrates a diagnostic limitation: absolute equality of writer timestamp
and element mediaTime is not a reliable universal frame-identity assumption.
`analyze-presentation-timing.py` now reports presentation count, unmatched count,
and an explicit incomplete-mapping warning rather than summarizing a silently
selected subset as proof of correct playout. Pixel timestamps and compositor
screenshots supply independent evidence in this synthetic test. No automatic
four-second compensation or inferred timestamp correction was added to the app.

The native clients and Clumsy suites have stopped, and the private camera pages
have been closed. The archived raw captures, native diagnostics, browser samples,
screenshots, Clumsy event logs, and `run-summary.json` support these conclusions.
There was no two-machine test in this run. The observed reconnect failure and
misleading green indicator remain follow-up defects, not claimed fixed here.
