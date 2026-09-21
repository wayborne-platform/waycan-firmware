#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-telemetry}"
case "$mode" in
    stock) defaults=sdkconfig ;;
    telemetry) defaults='sdkconfig;sdkconfig.waycan' ;;
    *) printf 'Usage: bash tools/build-waycan.sh [stock|telemetry]\n' >&2; exit 2 ;;
esac
idf.py -B "build/$mode" -D "SDKCONFIG=build/$mode/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$defaults" build
idf.py -B "build/$mode" size
