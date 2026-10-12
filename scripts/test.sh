#!/bin/bash
# Native (host) build + test of the DSP wrapper. Run inside the Docker image
# (needs a native g++): see README.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/src"
OUT="$PROJECT_DIR/build/test_gresil"
mkdir -p "$PROJECT_DIR/build"
g++ -std=c++17 -O2 -ffast-math -fno-exceptions -fno-rtti -DTEST -I"$SRC_DIR" \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-local-typedefs -Wno-class-memaccess \
    "$PROJECT_DIR/tests/test_gresil.cpp" \
    "$SRC_DIR/beads/dsp/processor.cc" "$SRC_DIR/beads/dsp/synth.cc" \
    "$SRC_DIR/beads/dsp/delay_controller.cc" "$SRC_DIR/beads/dsp/delay_clock.cc" \
    "$SRC_DIR/beads/dsp/trigger_generator.cc" "$SRC_DIR/beads/dsp/mu_law.cc" \
    "$SRC_DIR/beads/resources.cc" "$SRC_DIR/stmlib/dsp/units.cc" \
    "$SRC_DIR/stmlib/utils/random.cc" \
    -o "$OUT" -lm -lpthread
"$OUT"
