#!/bin/sh
# install.sh: build XKS-Memory from source, test it, and install it as its own application,
# separate from this repository. Works from a fresh clone; nothing pre-compiled is used.
# Memories live in ~/XKS-memories ($XKSM_HOME): .index/agents.xksm (the search index) and one
# folder per session with a file per turn; never in the repository or the install prefix.
#   tools/install.sh [--prefix DIR] [--no-claude] [--no-test] [--port FILE] [--backfill] [--check]
#     --prefix DIR   install prefix (default ~/.local): DIR/bin/xksm, DIR/bin/xksm-tui, ...
#     --no-claude    do not register the Claude Code plugin
#     --no-test      skip the test suite (it runs before anything is installed)
#     --port FILE    record a semantic port (tools/port_build output) in the store
#     --backfill     import the existing Claude Code transcripts (~/.claude/projects)
#     --check        only check the build requirements and exit
set -eu
prefix="$HOME/.local"
claude_reg=1
run_tests=1
port=""
backfill=0
check_only=0
while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --no-claude) claude_reg=0; shift ;;
    --no-test) run_tests=0; shift ;;
    --port) port=$2; shift 2 ;;
    --backfill) backfill=1; shift ;;
    --check) check_only=1; shift ;;
    -h | --help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done
src=$(cd "$(dirname "$0")/.." && pwd)
build="$src/build-install"

# ---- requirements -------------------------------------------------------------------------
missing=""
have() { command -v "$1" > /dev/null 2>&1; }
cc=""
for c in "${CC-}" cc gcc clang; do
    if [ -n "$c" ] && have "$c"; then cc=$c; break; fi
done
[ -n "$cc" ] || missing="$missing c-compiler"
have cmake || missing="$missing cmake"
if have cmake; then
    v=$(cmake --version | sed -n 's/^cmake version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    set -- $v
    if [ "${1:-0}" -lt 3 ] || { [ "${1:-0}" -eq 3 ] && [ "${2:-0}" -lt 20 ]; }; then
        echo "cmake $1.$2 is too old: 3.20 or newer is needed" >&2
        missing="$missing cmake>=3.20"
    fi
fi
sqlite_ok=0
if [ -n "$cc" ]; then
    # the store needs SQLite >= 3.42 with FTS5 (secure-delete): compile and run a probe
    probe=$(mktemp -d)
    cat > "$probe/p.c" << 'EOF'
#include <sqlite3.h>
#include <stdio.h>
int main(void)
{
    sqlite3 *db;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return 1;
    int ok = sqlite3_libversion_number() >= 3042000 &&
             sqlite3_exec(db, "CREATE VIRTUAL TABLE t USING fts5(x);"
                              "INSERT INTO t(t, rank) VALUES('secure-delete', 1);", 0, 0, 0) == SQLITE_OK;
    printf("%s\n", sqlite3_libversion());
    sqlite3_close(db);
    return ok ? 0 : 2;
}
EOF
    if "$cc" -o "$probe/p" "$probe/p.c" -lsqlite3 > /dev/null 2>&1; then
        if sv=$("$probe/p"); then
            sqlite_ok=1
        else
            echo "SQLite $sv lacks what the store needs (3.42 or newer, with FTS5)" >&2
        fi
    fi
    find "$probe" -depth -delete
fi
[ "$sqlite_ok" = 1 ] || missing="$missing sqlite3-dev(>=3.42,FTS5)"
have cargo || echo "note: cargo not found: the terminal interface xksm-tui will not be built"
have ninja || echo "note: ninja not found: using the default CMake generator"
if [ "$run_tests" = 1 ] && ! have sqlite3; then
    echo "note: the sqlite3 command is not installed: the tests that inspect stores need it (or use --no-test)"
    missing="$missing sqlite3-cli"
fi
if [ -n "$missing" ]; then
    echo "missing:$missing" >&2
    id=$( (. /etc/os-release 2> /dev/null && echo "${ID:-} ${ID_LIKE:-}") || true)
    case " $id " in
    *" arch "* | *" cachyos "* | *" manjaro "*) echo "install with: sudo pacman -S --needed base-devel cmake ninja sqlite rust" >&2 ;;
    *" debian "* | *" ubuntu "*) echo "install with: sudo apt install build-essential cmake ninja-build libsqlite3-dev sqlite3 cargo" >&2 ;;
    *" fedora "* | *" rhel "*) echo "install with: sudo dnf install gcc cmake ninja-build sqlite-devel sqlite cargo" >&2 ;;
    *) echo "install a C compiler, CMake >= 3.20, SQLite >= 3.42 headers (with FTS5), the sqlite3 command, and optionally ninja and cargo" >&2 ;;
    esac
    exit 1
fi
echo "== requirements: $cc, $(cmake --version | head -n 1), SQLite $sv$(have cargo && echo ", $(cargo --version)")"
[ "$check_only" = 1 ] && exit 0

# ---- build, test, install -------------------------------------------------------------------
gen=""
if have ninja; then
    if [ ! -f "$build/CMakeCache.txt" ] || grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$build/CMakeCache.txt"; then
        gen="-G Ninja"
    fi
fi
echo "== build ($build, Release)"
# shellcheck disable=SC2086
cmake -S "$src" -B "$build" $gen -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --parallel
if [ "$run_tests" = 1 ]; then
    echo "== test"
    if ! ctest --test-dir "$build" --output-on-failure > "$build/test.log" 2>&1; then
        tail -n 40 "$build/test.log" >&2
        echo "tests failed: nothing installed (full log: $build/test.log)" >&2
        exit 1
    fi
    tail -n 3 "$build/test.log" | sed -n 1p
fi
echo "== install to $prefix"
cmake --install "$build" --prefix "$prefix" > /dev/null
bin="$prefix/bin/xksm"
"$bin" version
[ -x "$prefix/bin/xksm-tui" ] && echo "xksm-tui installed: $prefix/bin/xksm-tui"
root="${XKSM_HOME:-$HOME/XKS-memories}"
mkdir -p "$root/.index" && chmod 700 "$root" "$root/.index"
# a store from before 0.2 (~/.local/share/xksm): moved, not copied (same inodes), then its
# session files are written
old="${XDG_DATA_HOME:-$HOME/.local/share}/xksm"
if [ -f "$old/agents.xksm" ] && [ ! -e "$root/.index/agents.xksm" ]; then
    echo "== move the memories from $old to $root/.index"
    for f in "$old"/agents.xksm*; do mv -n "$f" "$root/.index/"; done
    "$bin" mirror
fi
if [ -n "$port" ]; then
    echo "== record port $port"
    store="$root/.index/agents.xksm"
    "$bin" init "$store" > /dev/null
    "$bin" port "$store" "$port"
fi
if [ "$claude_reg" = 1 ] && have claude; then
    echo "== register the Claude Code plugin"
    claude plugin marketplace add "$prefix/share/xks-memory/claude-code" 2> /dev/null ||
        claude plugin marketplace update xks-memory || true
    claude plugin install xks-memory@xks-memory 2> /dev/null || claude plugin update xks-memory@xks-memory || true
elif [ "$claude_reg" = 1 ]; then
    echo "claude CLI not found: register later with"
    echo "  claude plugin marketplace add $prefix/share/xks-memory/claude-code"
    echo "  claude plugin install xks-memory@xks-memory"
fi
if [ "$backfill" = 1 ]; then
    echo "== backfill Claude Code transcripts"
    "$bin" transcripts
fi
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "note: $prefix/bin is not on PATH" ;; esac
echo "== done: application in $prefix/share/xks-memory, memories in $root (private, 700)"
