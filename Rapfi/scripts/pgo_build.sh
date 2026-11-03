#!/usr/bin/env bash
# BUILD-01: full PGO cycle for one configure preset (GCC or Clang, Linux/macOS).
#   1. build an instrumented binary   (PGO_MODE=GENERATE)
#   2. train it on `BENCH`, merge the profile
#   3. rebuild with the profile       (PGO_MODE=USE), LTO on
#
# Usage: scripts/pgo_build.sh [preset] [train_runs]
#   preset      configure preset from CMakePresets.json (default: x64-clang-AVX512VNNI)
#   train_runs  number of BENCH runs used for training (default: 1)
# Output: build/<preset>-pgo-use/pbrain-rapfi
# The profile is tied to the exact sources and compiler; re-run after any source change.

set -euo pipefail

PRESET=${1:-x64-clang-AVX512VNNI}
RUNS=${2:-1}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GEN_DIR="$ROOT/build/$PRESET-pgo-gen"
USE_DIR="$ROOT/build/$PRESET-pgo-use"
PROF_DIR="$GEN_DIR/profile"
LOG="$ROOT/build/$PRESET-pgo-use.log"

die() { echo "pgo_build: $*" >&2; exit 1; }

cd "$ROOT"

# Compiler family decides the profile format.
case "$PRESET" in *clang*) FAMILY=clang ;; *gcc*|*gnu*) FAMILY=gcc ;; *) FAMILY=${PGO_FAMILY:-clang} ;; esac
echo "pgo_build: preset=$PRESET family=$FAMILY runs=$RUNS"

if [ "$FAMILY" = clang ]; then
    PROFDATA=${LLVM_PROFDATA:-$(command -v llvm-profdata || true)}
    [ -n "$PROFDATA" ] || die "llvm-profdata not found (set LLVM_PROFDATA)"
fi

rm -rf "$GEN_DIR" "$USE_DIR"

echo "pgo_build: [1/3] instrumented build"
cmake --preset "$PRESET" -B "$GEN_DIR" -DENABLE_LTO=ON -DPGO_MODE=GENERATE >/dev/null
cmake --build "$GEN_DIR" --target rapfi

echo "pgo_build: [2/3] training on BENCH x$RUNS"
mkdir -p "$PROF_DIR"
for i in $(seq 1 "$RUNS"); do
    if [ "$FAMILY" = clang ]; then
        echo BENCH | LLVM_PROFILE_FILE="$PROF_DIR/rapfi-%p.profraw" "$GEN_DIR/pbrain-rapfi" >/dev/null
    else
        # GCC writes .gcda next to the object files; -fprofile-update is not needed for BENCH.
        echo BENCH | "$GEN_DIR/pbrain-rapfi" >/dev/null
    fi
done

if [ "$FAMILY" = clang ]; then
    ls "$PROF_DIR"/*.profraw >/dev/null 2>&1 || die "no .profraw produced by the instrumented binary"
    "$PROFDATA" merge -output="$PROF_DIR/rapfi.profdata" "$PROF_DIR"/*.profraw
    PROFILE="$PROF_DIR/rapfi.profdata"
else
    find "$GEN_DIR" -name '*.gcda' | grep -q . || die "no .gcda produced by the instrumented binary"
    PROFILE="$GEN_DIR"
fi

echo "pgo_build: [3/3] optimized build using $PROFILE"
cmake --preset "$PRESET" -B "$USE_DIR" -DENABLE_LTO=ON -DPGO_MODE=USE -DPGO_PROFILE="$PROFILE" >/dev/null
cmake --build "$USE_DIR" --target rapfi 2>&1 | tee "$LOG"

# A stale/mismatched profile is only a warning; do not ship such a binary silently.
if grep -qE 'profile-instr-(out-of-date|unprofiled)|profile data may be (incomplete|malformed)|no profile data available|coverage-mismatch' "$LOG"; then
    die "profile did not match the sources (see $LOG)"
fi

echo "pgo_build: done -> $USE_DIR/pbrain-rapfi"
echo "pgo_build: verify with: echo BENCH | $USE_DIR/pbrain-rapfi   (Nodes/Hash must match docs/process/change-process.md)"
