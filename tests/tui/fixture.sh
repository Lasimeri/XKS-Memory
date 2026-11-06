#!/bin/sh
# fixture.sh: the deterministic store of the TUI golden tests, built with the xksm CLI:
# placeholder text only, fixed event times, two harnesses, three sessions, a note.
# usage: fixture.sh XKSM_CLI STORE
set -eu
X=$1
S=$2
"$X" init "$S" > /dev/null
t=1793887320000000 # 2026-11-05 14:02:00 UTC
turn() { # turn HARNESS SESSION ROLE SECONDS TEXT
    printf '%s' "$5" | XKSM_STORE=$S "$X" turn --harness "$1" --session "$2" --role "$3" --cwd /tmp/placeholder-project --time-us $((t + $4 * 1000000))
}
turn claude-code sess-a user 0 "alpha placeholder question about lattice cache eviction"
turn claude-code sess-a thinking 2 "placeholder reasoning beta about the eviction order"
turn claude-code sess-a tool_use 3 "tool call: Read file_path: /tmp/placeholder.c"
turn claude-code sess-a tool_result 4 "placeholder file body gamma"
turn claude-code sess-a assistant 9 "placeholder answer: the lattice cache eviction uses CLOCK"
turn claude-code sess-b user 3600 "second placeholder session asks about vector snapshots"
turn claude-code sess-b assistant 3605 "placeholder answer: snapshots are mapped beside the index"
turn omp o-one user 7200 "omp placeholder turn about lattice cache eviction"
turn omp o-one assistant 7204 "omp placeholder reply zeta"
