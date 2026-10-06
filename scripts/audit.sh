#!/bin/bash
# Offline QoL audit of all 24 effects (tests/fx_audit.c) in a native Docker container.
# Output: tests/out/report.txt + WAV renders (knob spins, music sweeps, select/preset walks).
# Usage: ./scripts/audit.sh [effect_id]     (no id = everything)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."
ROOT_WIN="$(pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 docker build -q -t palette-audit -f tests/Dockerfile tests >/dev/null
CID=$(MSYS_NO_PATHCONV=1 docker create -w /build palette-audit bash -c "
    set -e
    dos2unix -q /build/src/dsp/*.c /build/src/dsp/*.cc /build/tests/*.c 2>/dev/null || true
    mkdir -p /build/obj /build/tests/out
    F='-O2 -ffast-math -fno-finite-math-only -I/build/src/dsp'
    gcc \$F -c /build/tests/fx_audit.c -o /build/obj/fx_audit.o
    gcc \$F -c /build/src/dsp/warps_data.c -o /build/obj/warps_data.o
    g++ \$F -std=c++11 -fno-exceptions -fno-rtti -I/build/vendor/clouds_engine -c /build/src/dsp/fx_clouds.cc -o /build/obj/fx_clouds.o
    g++ -o /build/fx_audit /build/obj/*.o -lm
    /build/fx_audit /build/tests/out $*
")
docker cp "$ROOT_WIN/src" "$CID:/build/src"
docker cp "$ROOT_WIN/tests" "$CID:/build/tests"
docker cp "$ROOT_WIN/vendor" "$CID:/build/vendor"
docker start -a "$CID"
EXIT_CODE=$(docker inspect "$CID" --format='{{.State.ExitCode}}')
rm -rf tests/out && docker cp "$CID:/build/tests/out" "$ROOT_WIN/tests/out" 2>/dev/null || true
docker rm "$CID" >/dev/null
[ "$EXIT_CODE" = "0" ] || { echo "audit failed (exit $EXIT_CODE)"; exit 1; }
echo "Report: tests/out/report.txt"
