#!/usr/bin/env bash
# Build VOLTKIT for MPC OS standalone on the MPC Live III (Gen2, aarch64, MPC OS 3.9.x).
#
#   bash mpc/build.sh            # -> mpc/build/dist/VOLTKIT-<version>-mpc-aarch64.zip
#
# Needs: zig (cross compiler, glibc-versioned aarch64 Linux target), cmake,
# ninja, python3 and a C compiler for the build machine. No Docker.
#
# The skin generator, VST2 wrapper and packager come from mpc-vst-plugins
# (the community Open MPC project), checked out at a pinned commit into
# mpc/.mpc-vst-plugins unless MPC_VST points at an existing checkout. The
# same setup as PerkyBits' mpc/build.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
VERSION="${VOLTKIT_MPC_VERSION:-0.1.0}"
MPC_VST_URL=https://github.com/sd88me/mpc-vst-plugins.git
MPC_VST_REF=28da1ce2a75f33b5ef802d0fadb0293862aed8a5
MPC_VST="${MPC_VST:-$HERE/.mpc-vst-plugins}"
GLIBC=2.36   # Live III runs glibc 2.39; stay below it

for tool in zig cmake ninja python3 cc; do
    command -v "$tool" >/dev/null || { echo "error: $tool not found" >&2; exit 1; }
done

# 1. mpc-vst-plugins at the pinned commit
if [ ! -d "$MPC_VST/.git" ]; then
    git clone --quiet "$MPC_VST_URL" "$MPC_VST"
fi
if [ "$(git -C "$MPC_VST" rev-parse HEAD)" != "$MPC_VST_REF" ]; then
    git -C "$MPC_VST" fetch --quiet origin "$MPC_VST_REF" 2>/dev/null || git -C "$MPC_VST" fetch --quiet origin
    git -C "$MPC_VST" checkout --quiet "$MPC_VST_REF"
fi

mkdir -p "$BUILD"

# 1b. the wrapper, patched to pass effMainsChanged on as set_param("mains"):
#     MPC suspends and resumes plug-ins on every STOP press, and VOLTKIT
#     stops the LXR on two of them (MPC's own double-STOP). And its state
#     buffer grows from 8 KB to 4 MB: the state holds the track's whole card.
mkdir -p "$BUILD/wrapper"
python3 - "$MPC_VST/wrapper/vst2_wrap.c" "$BUILD/wrapper/vst2_wrap.c" <<'PY'
import sys
src = open(sys.argv[1]).read()
old = "case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;"
new = ("case effSetSampleRate: case effSetBlockSize: return 1;\n"
       "    case effMainsChanged: eng_set(w, \"mains\", v ? \"1\" : \"0\"); return 1;   /* VOLTKIT: MPC's STOP */")
if src.count(old) != 1:
    sys.exit("build.sh: vst2_wrap.c changed; update the effMainsChanged patch")
src = src.replace(old, new)
old = "    char chunk[8192];"
if src.count(old) != 1:
    sys.exit("build.sh: vst2_wrap.c changed; update the chunk size patch")
src = src.replace(old, "    char chunk[4 << 20];   /* VOLTKIT: the card */")
open(sys.argv[2], "w").write(src)
PY

# 2. zig as an aarch64-linux-gnu cross toolchain (the wrappers drop the glibc
#    version when only preprocessing, which zig rejects)
TC="$BUILD/zig-aarch64"
mkdir -p "$TC"
for lang in cc c++; do
    out="$TC/z$lang"
    cat > "$out" <<EOF
#!/bin/sh
T=aarch64-linux-gnu.$GLIBC
for a in "\$@"; do [ "\$a" = "-E" ] && T=aarch64-linux-gnu; done
exec zig $lang -target \$T "\$@"
EOF
    chmod +x "$out"
done
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$TC/zar"
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$TC/zranlib"
chmod +x "$TC/zar" "$TC/zranlib"
cat > "$TC/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "$TC/zcc")
set(CMAKE_CXX_COMPILER "$TC/zc++")
set(CMAKE_AR "$TC/zar")
set(CMAKE_RANLIB "$TC/zranlib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF

# 3. VOLTKIT's panel (params.json, layout.conf) from gen_layout.py, then
#    params.h, the touchscreen skin and the plug-in list entry (gen_vst.py)
python3 "$HERE/gen_layout.py"
if [ ! -x "$BUILD/venv/bin/python" ]; then
    python3 -m venv "$BUILD/venv"
    "$BUILD/venv/bin/pip" install --quiet pillow
fi
mkdir -p "$BUILD/fonts"
cp "$MPC_VST"/tools/html_art/fonts/TitilliumWeb-*.ttf "$BUILD/fonts/"
cc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o "$BUILD/shadow_art" "$MPC_VST/tools/shadow_art.c" -lm
(cd "$HERE" && SHADOW_TITLE_FONT="$BUILD/fonts/TitilliumWeb-SemiBold.ttf" \
    "$BUILD/venv/bin/python" "$MPC_VST/tools/gen_vst.py" vst.json)

# 4. the plug-in, the engine library and the tests, cross-compiled
cmake -S "$HERE" -B "$BUILD/aarch64" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TC/toolchain.cmake" \
    -DMPC_VST_DIR="$MPC_VST" \
    -DMPC_PARAMS_DIR="$BUILD" \
    -DMPC_WRAPPER_SOURCE="$BUILD/wrapper/vst2_wrap.c" >/dev/null
cmake --build "$BUILD/aarch64" --parallel
SO="$BUILD/aarch64/voltkit.so"
ENGINE="$BUILD/aarch64/voltkit-engine.so"
echo "built $SO and $ENGINE"
EXPORTED="$(nm -D --defined-only --extern-only "$SO" | awk '{print $NF}' | tr '\n' ' ')"
echo "plug-in exports: $EXPORTED"
[ "$EXPORTED" = "VSTPluginMain " ] || { echo "error: the plug-in must export only VSTPluginMain" >&2; exit 1; }
EXPORTED="$(nm -D --defined-only --extern-only "$ENGINE" | awk '{print $NF}' | tr '\n' ' ')"
echo "engine exports: $EXPORTED"
[ "$EXPORTED" = "volt_machine_api " ] || { echo "error: the engine must export only volt_machine_api" >&2; exit 1; }
for f in "$SO" "$ENGINE"; do
    echo "$(basename "$f"): highest glibc $(strings "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1) (Live III has 2.39)"
done

# 5. one zip with the plug-in folder, install.sh / uninstall.sh and INSTALL.md.
#    engine/ holds the engine library with the GPL text and where its source
#    is (it is built from the GPL-3.0 LXR firmware).
SKIN="$BUILD/skin/voltkit - VST - VOLTKIT"
rm -rf "$BUILD/extra"
mkdir -p "$BUILD/extra/data" "$BUILD/extra/engine"
cat > "$BUILD/extra/data/README.txt" <<'EOF2'
Reinstalling VOLTKIT keeps this folder. Each track's LXR card (its kits,
patterns and settings) is saved inside the MPC project, not here.
EOF2
cp "$ENGINE" "$BUILD/extra/engine/"
cp "$HERE/../data/card.vkc" "$BUILD/extra/engine/"   # the card new tracks start with
cp "$HERE/../LICENSE" "$BUILD/extra/engine/LICENSE.txt"
cp "$HERE/../firmware/NOTICE.md" "$BUILD/extra/engine/LXR-NOTICE.md"
cat > "$BUILD/extra/engine/SOURCE.txt" <<EOF2
VOLTKIT $VERSION is free software under the GNU General Public License,
version 3 (LICENSE.txt). It runs the LXR V2 firmware by Erica Synths
(GPL-3.0, see LXR-NOTICE.md) on VOLTKIT's own virtual board; the default card
(card.vkc) is the firmware's own Init project. VOLTKIT is not made, supported
or endorsed by Erica Synths.

Source: https://github.com/jrold/lxr-mpc (commit $(git -C "$HERE/.." rev-parse --short HEAD))
EOF2
rm -rf "$BUILD/dist"
python3 "$MPC_VST/tools/release.py" --so "$SO" --skin "$SKIN" --entry "$BUILD/pluginlist-entry.xml" \
    --version "$VERSION" --extra "$BUILD/extra/data:data" --extra "$BUILD/extra/engine:engine" --user-data data \
    --license GPL-3.0-only --repo jrold/lxr-mpc \
    --about "The LXR-02 drum machine's open-source firmware on a virtual board: its sequencer, parameter locks, menus and seven voices, on the touchscreen." \
    -o "$BUILD/dist"
ls -la "$BUILD/dist"
