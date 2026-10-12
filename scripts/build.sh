#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/src"
BUILD_DIR="$PROJECT_DIR/build"
OUT_DIR="$PROJECT_DIR/modules/gresil"

# Cross-compiler prefix (Docker or local toolchain)
CC="${CROSS_PREFIX:-aarch64-linux-gnu-}g++"

echo "=== Building Gresil for Ableton Move (ARM64) ==="
echo "Compiler: $CC"

mkdir -p "$BUILD_DIR" "$OUT_DIR"

SOURCES=(
    "$SRC_DIR/gresil_move.cpp"
    "$SRC_DIR/beads/dsp/processor.cc"
    "$SRC_DIR/beads/dsp/synth.cc"
    "$SRC_DIR/beads/dsp/delay_controller.cc"
    "$SRC_DIR/beads/dsp/delay_clock.cc"
    "$SRC_DIR/beads/dsp/trigger_generator.cc"
    "$SRC_DIR/beads/dsp/mu_law.cc"
    "$SRC_DIR/beads/resources.cc"
    "$SRC_DIR/stmlib/dsp/units.cc"
    "$SRC_DIR/stmlib/utils/random.cc"
)

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename "$src" | sed 's/\.\(cpp\|cc\|c\)$/.o/')"
    echo "  CC $src"
    $CC -c "$src" -o "$obj" \
        -I"$SRC_DIR" \
        -std=c++17 \
        -O2 -fPIC -ffast-math \
        -march=armv8-a -mtune=cortex-a72 \
        -fomit-frame-pointer -fno-stack-protector \
        -fno-exceptions -fno-rtti \
        -fno-tree-loop-distribute-patterns \
        -fvisibility=hidden \
        -DNDEBUG -DTEST \
        -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
        -Wno-unused-local-typedefs -Wno-class-memaccess
    OBJECTS+=("$obj")
done

echo "  LD gresil.so"
$CC -shared -o "$OUT_DIR/gresil.so" "${OBJECTS[@]}" -lm -lpthread
chmod +x "$OUT_DIR/gresil.so"

cp "$SRC_DIR/module.json" "$OUT_DIR/"
cp "$SRC_DIR/help.json" "$OUT_DIR/"

# Fail loudly if the exports are missing (a stale .so has shipped before).
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}nm" >/dev/null 2>&1; then
    NM="${CROSS_PREFIX:-aarch64-linux-gnu-}nm"
    for sym in move_audio_fx_init_v2 move_audio_fx_on_midi; do
        "$NM" -D --defined-only "$OUT_DIR/gresil.so" | grep -q " $sym$" \
            || { echo "ERROR: $sym not exported"; exit 1; }
    done
fi

echo "=== Build complete: $OUT_DIR/gresil.so ==="
ls -la "$OUT_DIR/"
