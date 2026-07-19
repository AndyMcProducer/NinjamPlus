#!/bin/sh

set -eu

ROOT=${1:?"usage: validate-macos-universal.sh <build-output-root>"}
BINARIES=$(find "${ROOT}" -type f -path '*/Contents/MacOS/*' -perm -111 -print)

if [ -z "${BINARIES}" ]; then
  echo "No packaged macOS executables found under ${ROOT}" >&2
  exit 1
fi

COUNT=$(printf '%s\n' "${BINARIES}" | wc -l | tr -d ' ')
if [ "${COUNT}" -lt 3 ]; then
  echo "Expected Standalone, AU, and VST3 executables; found ${COUNT}" >&2
  exit 1
fi

printf '%s\n' "${BINARIES}" | while IFS= read -r BINARY; do
  ARCHS=$(lipo -archs "${BINARY}")
  case " ${ARCHS} " in
    *" x86_64 "*) ;;
    *) echo "Missing x86_64 slice: ${BINARY} (${ARCHS})" >&2; exit 1 ;;
  esac
  case " ${ARCHS} " in
    *" arm64 "*) ;;
    *) echo "Missing arm64 slice: ${BINARY} (${ARCHS})" >&2; exit 1 ;;
  esac
  echo "Universal binary verified: ${BINARY} (${ARCHS})"
done
