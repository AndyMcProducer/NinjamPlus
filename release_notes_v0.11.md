Session recording, VDO sync buffer fix, voice channel index fix, Auto Level improvements

- Add session recording: records master, local, and remote audio as
  separate per-track WAV files in a timestamped folder
- Add Record button with pulsing red glow while recording is active
- Add Record Folder button to choose output directory (remembered
  across sessions)
- Support recording continuation after disconnect/reconnect within
  60 seconds to the same server; stop on different server or after
  60 seconds disconnected
- Stop recording when Record button clicked again
- Fix VDO sync buffer double-counting network route latency: the
  measured elapsed time already includes the full end-to-end path
  (remote user -> server -> us), so serverRouteLatencyMs was being
  added on top incorrectly in three places (firm delay path and
  two fallback paths)
- Fix late-arriving sync tag retransmissions being permanently
  blocked: when a sync tag arrived late (tagIsLate), the marker key
  was still recorded in lastAnnouncedRemoteIntervalByUser, causing
  redundant retransmissions for the same interval to be treated as
  duplicates and dropped — now the marker key is only recorded when
  the pending entry is actually stored
- Fix voice chat channel index: voice was hardcoded to NINJAM
  channel index 4, skipping index 3 entirely — with MaxChannels=4
  (indices 0-3), voice could never fit even though there was a
  free slot; changed kVoiceChatChannelIndex from 4 to 3 so the
  layout is now audio 0, control 1, Opus 2, voice 3
- Improve Auto Level: primarily raises quiet users, restores
  original levels when disabled, defaults users who joined while
  Auto Level was enabled to 1.0f (0 dB) if no baseline exists
- Fix non-Opus remote LUFS meter not updating when chord analysis
  is enabled (metering now runs before chord-analysis branching)
- Add Max Ch: N label showing server maximum local channels
- Fix mixer/user-list popout restoration by forcing complete
  resized() layout pass
- Add configurable DPI settings: Auto, 50%, 75%, 100%, 125%, 150%
- Expand plugin inputs to support up to 16 input buses (32 mono
  channels or 16 stereo buses)
- Keep Auto-Tune button visible when only one local channel is
  available (server lacks free channels)
- Move Record button to top row toward the right
- Improve VDO sync messaging and retransmit mobile-hotspot side
  signals only while active VDO synchronization is enabled
