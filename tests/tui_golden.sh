#!/bin/sh
# tui_golden.sh: headless golden test of xksm-tui. Builds the fixture store with the CLI, runs
# the key scripts in tests/tui and compares every screen (text + styled runs) byte for byte with
# tests/tui/golden. The script runs twice, once in a UTF-8 locale and once in the C locale (the
# built-in width table must give the same frames), and the minimum-size screen once.
# usage: tui_golden.sh XKSM_TUI XKSM_CLI SCRATCH_DIR   (UPDATE=1 rewrites the golden files)
set -u
abs() { case $1 in /*) echo "$1" ;; *) echo "$(pwd)/$1" ;; esac; }
T=$(abs "$1")
XKSM_CLI=$(abs "$2")
D=$(abs "$3")
G=$(cd "$(dirname "$0")/tui" && pwd)
export XKSM_CLI TZ=UTC XKSM_HOME=root XKSM_MIRROR=0
unset XKSM_STORE NO_COLOR
mkdir -p "$D"
fails=0
run() { # run NAME LOCALE SIZE SCRIPT
    w=$D/$1
    [ -d "$w" ] && find "$w" -depth -delete
    mkdir -p "$w/snap"
    (cd "$w" && sh "$G/fixture.sh" "$XKSM_CLI" fixture.xksm &&
        LC_ALL=$2 XKSM_STORE=fixture.xksm "$T" --script "$G/$4" --snapshot snap --size "$3" fixture.xksm > /dev/null) ||
        { echo "FAIL: $1: the run itself failed"; fails=$((fails + 1)); return; }
}
run utf8 C.UTF-8 200x50 golden.script
run c C 200x50 golden.script
run small C.UTF-8 79x19 small.script
if [ "${UPDATE-}" = 1 ] && [ "$fails" = 0 ]; then
    find "$G/golden" -name '*.txt' -type f -delete
    cp "$D"/utf8/snap/*.txt "$D"/small/snap/*.txt "$G/golden/"
    echo "tui_golden: golden files rewritten ($(ls "$G/golden" | wc -l | tr -d ' '))"
    exit 0
fi
n=0
for f in "$G"/golden/*.txt; do
    b=$(basename "$f")
    for r in utf8 c small; do
        [ -f "$D/$r/snap/$b" ] || continue
        n=$((n + 1))
        if ! cmp -s "$f" "$D/$r/snap/$b"; then
            fails=$((fails + 1))
            echo "FAIL: $r/$b differs from the golden file:"
            diff "$f" "$D/$r/snap/$b" | head -n 20
        fi
    done
done
want=$(($(ls "$G"/golden/*.txt | wc -l) * 2 - 1))
[ "$n" = "$want" ] || { echo "FAIL: $n screens compared, expected $want"; fails=$((fails + 1)); }
echo "tui_golden: $((n - fails))/$n screens equal to the golden files"
[ "$fails" = 0 ]
