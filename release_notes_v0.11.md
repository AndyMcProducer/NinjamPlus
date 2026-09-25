Hidden sync transport fallbacks for vanilla servers

- Fall back to transports that never surface in vanilla ReaNINJAM chat
  when the server drops SIDE_SIGNAL or cannot host the hidden control
  channel: interval-channel smuggle under the NJS4 fourcc, then
  private messages between confirmed NINJAMplus peers
- Smuggle sync signals through the audio channel (index 0) under the
  sync fourcc on servers that only allow one local channel; servers
  relay unknown fourccs transparently and vanilla clients discard
  unknown interval payloads silently
- Relay signals as targeted PRIVMSG only to peers confirmed to run
  this client, so vanilla users are never sent sync payloads and
  cannot display them
- Ignore our own chat-relay echo, honour the optional target filter
  on chat-relayed signals, and swallow every prefixed relay line so
  sync payloads never render in the chat window
- Route chat-relayed sync signals through the same interval pipeline
  as side signals, labelled CHAT in the sync diagnostics
