#!/bin/bash
set -e
MODULE_ID="palette"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
cd "$ROOT"                          # run from repo root so a relative build context works
ROOT_WIN="$(pwd -W 2>/dev/null || pwd)"   # Windows-style path for docker cp on MSYS
IMAGE="${MODULE_ID}-builder"

echo "Building $MODULE_ID for ARM64 (aarch64)..."
# Relative "." context avoids MSYS mangling an absolute /c/... path into an invalid path.
MSYS_NO_PATHCONV=1 docker build -t "$IMAGE" -f scripts/Dockerfile .

# Create a container, copy sources in, build, copy the .so out (cross-platform safe).
# PALETTE may mix C (host + sibling-derived effects) and C++ (Mutable Warps/Clouds for
# FOLD/SHIFT/BLOOM/SPACE). Compile each TU with the right driver, link with g++ for libstdc++.
CID=$(MSYS_NO_PATHCONV=1 docker create -w /build "$IMAGE" bash -c '
    set -e
    shopt -s nullglob
    dos2unix /build/src/dsp/*.c /build/src/dsp/*.cc /build/src/dsp/*.cpp 2>/dev/null || true
    mkdir -p /build/dist/palette /build/obj
    CFLAGS="-O2 -fPIC -ffast-math -I/build/src/dsp"
    CXXFLAGS="-O2 -fPIC -ffast-math -std=c++11 -fno-exceptions -fno-rtti -I/build/src/dsp -I/build/vendor/clouds_engine -I/build/vendor/signalsmith"
    OBJS=()
    for f in /build/src/dsp/*.c; do
        o="/build/obj/$(basename "${f%.c}").o"
        echo "CC  $f"; aarch64-linux-gnu-gcc $CFLAGS -c "$f" -o "$o"; OBJS+=("$o")
    done
    for f in /build/src/dsp/*.cc /build/src/dsp/*.cpp; do
        o="/build/obj/$(basename "${f%.*}").o"
        echo "CXX $f"; aarch64-linux-gnu-g++ $CXXFLAGS -c "$f" -o "$o"; OBJS+=("$o")
    done
    echo "LINK -> palette.so"
    aarch64-linux-gnu-g++ -O2 -shared -fPIC -o /build/dist/palette/palette.so "${OBJS[@]}" -lm
    cp /build/src/module.json /build/dist/palette/
    [ -f /build/src/help.json ] && cp /build/src/help.json /build/dist/palette/ || true
    ls -la /build/dist/palette/
')
docker cp "$ROOT_WIN/src" "$CID:/build/src"
# vendor/ holds fetched third-party DSP (Mutable Warps/Clouds, etc.); copy if present.
[ -d vendor ] && docker cp "$ROOT_WIN/vendor" "$CID:/build/vendor" || true
docker start -a "$CID"
# set -e does NOT catch docker failures on Git Bash/MSYS — check explicitly or we
# silently deploy the previous build's stale .so.
EXIT_CODE=$(docker inspect "$CID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: compile failed inside container (exit $EXIT_CODE). See output above."
    docker rm "$CID" >/dev/null 2>&1 || true
    exit 1
fi
mkdir -p dist/palette
docker cp "$CID:/build/dist/palette/palette.so" "$ROOT_WIN/dist/palette/"
docker cp "$CID:/build/dist/palette/module.json" "$ROOT_WIN/dist/palette/"
docker cp "$CID:/build/dist/palette/help.json" "$ROOT_WIN/dist/palette/" 2>/dev/null || true
docker rm "$CID" >/dev/null
echo "Built: dist/palette/"
