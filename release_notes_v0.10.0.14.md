Mobile hotspot VDO sync, SSH tunnel option, late sync tag handling

- Add Mobile Hotspot Mode: forces VDO.Ninja video through TURN relay
  over TCP/443 (&relay &tcp) to bypass UDP blocking on mobile hotspots
  and heavy firewalls — no external server needed, one-click toggle
- Add redundant sync tag transmission in Mobile Hotspot Mode: interval
  sync tags sent 3x (0ms, +150ms, +300ms) with sendOffsetMs field so
  receiver can correct timing on duplicate arrivals
- Fix late-arriving sync tags producing wrong buffer calculations:
  tags from past intervals are no longer stored as pending, preventing
  buffer offsets of a full interval; existing firm delay persists
- Clean up stale pending sync entries from past intervals
- Add SSH tunnel option for advanced users with their own VPS relay
  (routes NINJAM connection through ssh -L local forward)
- Add SSH tunnel settings dialog with visible modal, desktop-attached
  window, async startup on background thread, BatchMode=yes
- Fix GUI reopen resetting live processor state: processor state is
  restored only on first editor open; subsequent GUI opens reload
  UI settings only, preserving active pads, connections, and FX
- Add Rubber Band offline stretching for sample pads (R3/finer engine,
  OptionDetectorPercussive, OptionTransientsCrisp)
- Add editable source BPM correction for sample pads
- Add scheduled loop playback: looped pads start at next appropriate
  NINJAM interval boundary (blue while waiting, green when playing)
- Defer playback speed change until next loop boundary when pad is playing
- Fix chat toggle: chat hides on main GUI when popped out
- Fix disabling BPM sync restores original sample
- Add librosa C++ with phase vocoder, beat tracking, tempo estimation
- Mobile hotspot keepalive every 500ms to maintain NAT connection
