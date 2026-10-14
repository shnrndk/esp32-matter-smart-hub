#!/usr/bin/env bash
set -e
source "$(dirname "$0")/env.sh"
idf.py -C "$SMARTHUB_ROOT/firmware" "$@" build
