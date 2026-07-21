#!/usr/bin/env bash
set -euo pipefail

# Optional first argument: codesign identity (default ad-hoc "-")
IDENTITY="${1:--}"

log() { printf '%s\n' "$*"; }

fix_bundle() {
  local bundle="$1"
  if [ ! -d "$bundle" ]; then
    return
  fi

  log ""
  log "Fixing: $bundle"

  # Remove quarantine flags on bundle and helper folders.
  sudo xattr -cr "$bundle" || true
  sudo xattr -cr "$bundle/Contents/MacOS/advanced-vdo-client" 2>/dev/null || true
  sudo xattr -cr "$bundle/Contents/Resources/advanced-vdo-client" 2>/dev/null || true

  # Re-sign bundled node helper if present.
  local node_a="$bundle/Contents/Resources/advanced-vdo-client/node"
  local node_b="$bundle/Contents/MacOS/advanced-vdo-client/node"
  if [ -x "$node_a" ]; then
    sudo xattr -cr "$node_a" || true
    sudo codesign --force --sign "$IDENTITY" "$node_a" || true
  fi
  if [ -x "$node_b" ]; then
    sudo xattr -cr "$node_b" || true
    sudo codesign --force --sign "$IDENTITY" "$node_b" || true
  fi

  # Re-sign bundle deeply so helper can execute under host.
  sudo codesign --force --deep --sign "$IDENTITY" "$bundle" || true
}

log "NINJAM VDO Fix"
log "Using signing identity: $IDENTITY"

VST3_USER_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_USER_DIR="$HOME/Library/Audio/Plug-Ins/Components"
VST3_SYSTEM_DIR="/Library/Audio/Plug-Ins/VST3"
AU_SYSTEM_DIR="/Library/Audio/Plug-Ins/Components"

fix_bundle "$VST3_USER_DIR/NINJAMplus.vst3"
fix_bundle "$VST3_USER_DIR/ninjamplus.vst3"
fix_bundle "$VST3_USER_DIR/NINJAM VST3.vst3"
fix_bundle "$VST3_SYSTEM_DIR/NINJAMplus.vst3"
fix_bundle "$VST3_SYSTEM_DIR/ninjamplus.vst3"
fix_bundle "$VST3_SYSTEM_DIR/NINJAM VST3.vst3"

fix_bundle "$AU_USER_DIR/NINJAMplus.component"
fix_bundle "$AU_USER_DIR/ninjamplus.component"
fix_bundle "$AU_USER_DIR/NINJAM VST3.component"
fix_bundle "$AU_SYSTEM_DIR/NINJAMplus.component"
fix_bundle "$AU_SYSTEM_DIR/ninjamplus.component"
fix_bundle "$AU_SYSTEM_DIR/NINJAM VST3.component"

log ""
log "Done."
log "Restart your DAW and try Video Room again."
