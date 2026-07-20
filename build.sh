#!/bin/sh

# Backwards-compatible entry point retained after build.sh became build-macos.sh.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "${SCRIPT_DIR}/build-macos.sh" "$@"
