#!/bin/sh
# The stage 2 equivalence check (README.md): builds the baseline hook (commit 47f6645) and the working tree's split
# build side by side and runs seeded sessions through both, in parallel. Exit 0 only if every session matches.
#
#   tests/equivalence/run.sh [--mutate core|follow|processor] [sessions] [frames]
#
# Needs g++ (C++23), git, tar and awk. From Windows: wsl -d archlinux -- sh tests/equivalence/run.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASELINE=47f6645
MUTATE=""
if [ "${1:-}" = "--mutate" ]; then
    MUTATE=$2
    shift 2
fi
SESSIONS=${1:-8}
FRAMES=${2:-300000}
CXX=${CXX:-g++}
BUILD="$HERE/build"

rm -rf "$BUILD"
mkdir -p "$BUILD/base" "$BUILD/new" "$BUILD/log"

# The baseline: 47f6645's sources, and its dllmain.cpp's hook region, from `namespace` to the end of camera_live.
git -C "$ROOT" archive "$BASELINE" src | tar -x -C "$BUILD/base"
awk '
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
    core) plant core/pipeline.hpp "double w = s * s * (3.0 - 2.0 * s);" "double w = s;" ;;          # the crossfade eased linearly
    follow) plant sw/follow/follow.hpp "m_nominal_hold = t.transition + 0.3;" "m_nominal_hold = t.transition + 0.35;" ;; # the wall-clamp hold
    processor) plant sw/processor.hpp "if (settings != self.m_seen_settings || sw.mode_write)" "if (settings != self.m_seen_settings)" ;; # mode writes do not fade
    *) echo "unknown mutation: $MUTATE"; exit 2 ;;
esac

FLAGS="-std=c++23 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -I$HERE -I$HERE/shim -I$BUILD"
$CXX $FLAGS -I"$BUILD/base/src" -Ddwapi=base_dwapi -Ddwsc=base_dwsc -Ddwsw=base_dwsw -Ddwcam=base_dwcam -c "$HERE/base_driver.cpp" -o "$BUILD/base.o" &
$CXX $FLAGS -I"$BUILD/new/src" -Ddwapi=new_dwapi -Ddwsc=new_dwsc -Ddwsw=new_dwsw -Ddwcam=new_dwcam -c "$HERE/new_driver.cpp" -o "$BUILD/new.o" &
$CXX $FLAGS -c "$HERE/main.cpp" -o "$BUILD/main.o" &
wait
[ -f "$BUILD/base.o" ] && [ -f "$BUILD/new.o" ] && [ -f "$BUILD/main.o" ] || { echo "build failed"; exit 1; }
$CXX -o "$BUILD/equivalence" "$BUILD/base.o" "$BUILD/new.o" "$BUILD/main.o"

echo "baseline $BASELINE vs working tree${MUTATE:+ (mutation: $MUTATE)}: $SESSIONS sessions x $FRAMES frames"
i=1
while [ "$i" -le "$SESSIONS" ]; do
    "$BUILD/equivalence" "$i" "$FRAMES" > "$BUILD/log/session-$i.txt" 2>&1 &
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
