#!/bin/sh
# The stage 2 equivalence check (README.md): builds the baseline hook (commit f22aa8e) and the working tree's split
# build side by side and runs seeded sessions through both, in parallel. Exit 0 only if every session matches.
#
#   tests/equivalence/run.sh [--mutate core|follow|processor] [--no-baseline-fixes] [sessions] [frames]
#
# The baseline gets the patches in baseline-fixes/ (the intended changes since f22aa8e) unless --no-baseline-fixes:
# that run must FAIL at the first event a fix changes. EQ_REKEY_CHANCE=p raises the per-event re-key chance (default
# 0.00001) so a verification run reaches re-keys that hold a layer.
#
# Needs g++ (C++23), git, tar, awk and patch. From Windows: wsl -d archlinux -- sh tests/equivalence/run.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASELINE=f22aa8e
MUTATE=""
FIXES=1
while :; do
    case "${1:-}" in
        --mutate) MUTATE=$2; shift 2 ;;
        --no-baseline-fixes) FIXES=0; shift ;;
        *) break ;;
    esac
done
SESSIONS=${1:-8}
FRAMES=${2:-300000}
REKEY=${EQ_REKEY_CHANCE:-}
CXX=${CXX:-g++}
BUILD="$HERE/build"

rm -rf "$BUILD"
mkdir -p "$BUILD/base" "$BUILD/new" "$BUILD/log"

# The baseline: f22aa8e's sources, and its dllmain.cpp's hook region, from `namespace` to the end of camera_live.
git -C "$ROOT" archive "$BASELINE" src | tar -x -C "$BUILD/base"
BASE_DEFS="-DEQ_BASELINE"
if [ "$FIXES" -eq 1 ]; then
    for p in "$HERE"/baseline-fixes/*.patch; do
        [ -f "$p" ] || continue
        patch -p1 -s -d "$BUILD/base" < "$p" || { echo "baseline fix does not apply: $p"; exit 1; }
        echo "baseline fix applied: $(basename "$p")"
    done
    BASE_DEFS="$BASE_DEFS -DEQ_BASELINE_FIXED"
else
    echo "baseline fixes NOT applied: expect a mismatch where a fixed behaviour differs"
fi
awk '
    { sub(/\r$/, "") }
    !started && $0 == "namespace" { started = 1 }
    started { print }
    started && /^    auto camera_live\(\) -> bool$/ { in_live = 1 }
    in_live && $0 == "    }" { exit }
' "$BUILD/base/src/dllmain.cpp" > "$BUILD/base_region.inc"
echo "} // namespace" >> "$BUILD/base_region.inc"
grep -q "void __fastcall get_camera_view_hook" "$BUILD/base_region.inc" || { echo "baseline region not found"; exit 1; }

# The split: the working tree's sources, copied so a planted mutation never touches them.
cp -R "$ROOT/src" "$BUILD/new/"
plant() { # file, from, to: replace one exact line fragment, or fail
    grep -qF -- "$2" "$BUILD/new/src/$1" || { echo "mutation anchor not found in $1: $2"; exit 1; }
    FROM=$2 TO=$3 awk '{ i = index($0, ENVIRON["FROM"]); if (i) $0 = substr($0, 1, i - 1) ENVIRON["TO"] substr($0, i + length(ENVIRON["FROM"])); print }' \
        "$BUILD/new/src/$1" > "$BUILD/mutated" && mv "$BUILD/mutated" "$BUILD/new/src/$1"
    echo "planted in $1: '$2' -> '$3'"
}
case "$MUTATE" in
    "") ;;
    core) plant camera/pipeline.hpp "double w = s * s * (3.0 - 2.0 * s);" "double w = s;" ;;          # the crossfade eased linearly
    follow) plant smoothwalker/follow/follow.hpp "m_nominal_hold = t.transition + 0.3;" "m_nominal_hold = t.transition + 0.35;" ;; # the wall-clamp hold
    processor) plant smoothwalker/follow/processor.hpp "if (settings != m_seen_settings || sw.mode_write)" "if (settings != m_seen_settings)" ;; # mode writes do not fade
    *) echo "unknown mutation: $MUTATE"; exit 2 ;;
esac

FLAGS="-std=c++23 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -I$HERE -I$HERE/shim -I$BUILD"
$CXX $FLAGS -I"$BUILD/base/src" $BASE_DEFS -Ddwapi=base_dwapi -Ddwsc=base_dwsc -Ddwsw=base_dwsw -Ddwcam=base_dwcam -c "$HERE/base_driver.cpp" -o "$BUILD/base.o" &
$CXX $FLAGS -I"$BUILD/new/src" -Ddw=new_dw -c "$HERE/new_driver.cpp" -o "$BUILD/new.o" &
$CXX $FLAGS -c "$HERE/main.cpp" -o "$BUILD/main.o" &
wait
[ -f "$BUILD/base.o" ] && [ -f "$BUILD/new.o" ] && [ -f "$BUILD/main.o" ] || { echo "build failed"; exit 1; }
$CXX -o "$BUILD/equivalence" "$BUILD/base.o" "$BUILD/new.o" "$BUILD/main.o"

echo "baseline $BASELINE$([ "$FIXES" -eq 1 ] && echo " + fixes") vs working tree${MUTATE:+ (mutation: $MUTATE)}: $SESSIONS sessions x $FRAMES frames${REKEY:+, re-key chance $REKEY}"
i=1
while [ "$i" -le "$SESSIONS" ]; do
    "$BUILD/equivalence" "$i" "$FRAMES" $REKEY >"$BUILD/log/session-$i.txt" 2>&1 &
    i=$((i + 1))
done
wait

FAILED=0
i=1
while [ "$i" -le "$SESSIONS" ]; do
    cat "$BUILD/log/session-$i.txt"
    grep -q "0 mismatches" "$BUILD/log/session-$i.txt" || FAILED=$((FAILED + 1))
    i=$((i + 1))
done
if [ "$FAILED" -ne 0 ]; then
    echo "FAIL: $FAILED of $SESSIONS sessions differ"
    exit 1
fi
echo "PASS: $SESSIONS sessions, identical to the byte"
