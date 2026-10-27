#!/usr/bin/env bash
# Install the built VOLTKIT package on a modded MPC Live III over SSH.
#
#   bash mpc/deploy.sh root@mpc-live-iii.local [port]
#
# Runs the package's own install.sh as root: it stops MPC, backs up
# MPC.settings, registers the plug-in under pluginList-arm-64bit and starts
# MPC again. Save your project first.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:?usage: deploy.sh user@host [port]}"
PORT="${2:-22}"
SYNTHS=/nvme/Synths   # the Live III's writable, exec-capable internal drive

ZIP="$(ls -t "$HERE"/build/dist/VOLTKIT-*-mpc-aarch64.zip 2>/dev/null | head -n 1)"
[ -n "$ZIP" ] || { echo "error: no package; run mpc/build.sh first" >&2; exit 1; }
SSH=(ssh -4 -p "$PORT" -o BatchMode=yes "$TARGET")

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
unzip -q "$ZIP" -d "$STAGE"
PKG="$(basename "$ZIP" -mpc-aarch64.zip)"

# COPYFILE_DISABLE keeps macOS tar from adding AppleDouble ._* files.
COPYFILE_DISABLE=1 tar -C "$STAGE" -cf - "$PKG" | "${SSH[@]}" 'rm -rf /tmp/voltkit-install && mkdir -p /tmp/voltkit-install && tar xf - -C /tmp/voltkit-install'
"${SSH[@]}" "cd '/tmp/voltkit-install/$PKG' && sh install.sh -y -t $SYNTHS 2>&1 | grep -v \"can't preserve ownership\"; rm -rf /tmp/voltkit-install"
