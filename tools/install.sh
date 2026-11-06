#!/bin/sh
# install.sh: build XKS-Memory and install it as its own application, separate from this
# repository. Memories live in ~/XKS-memories ($XKSM_HOME): .index/agents.xksm (the search index)
# and one folder per session with a file per turn; never here.
#   tools/install.sh [--prefix DIR] [--no-claude] [--port FILE] [--backfill]
#     --prefix DIR   install prefix (default ~/.local): DIR/bin/xksm, DIR/share/xksm/...
#     --no-claude    do not register the Claude Code plugin
#     --port FILE    record a semantic port (tools/port_build output) in the store
#     --backfill     import the existing Claude Code transcripts (~/.claude/projects)
set -eu
prefix="$HOME/.local"
claude_reg=1
port=""
backfill=0
while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --no-claude) claude_reg=0; shift ;;
    --port) port=$2; shift 2 ;;
    --backfill) backfill=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
src=$(cd "$(dirname "$0")/.." && pwd)
build="$src/build-install"
echo "== build ($build, Release)"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target xksm_cli xksm
echo "== install to $prefix"
cmake --install "$build" --prefix "$prefix" > /dev/null
bin="$prefix/bin/xksm"
"$bin" version
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
if command -v cargo > /dev/null 2>&1; then
    echo "== build and install the terminal interface xksm-tui"
    XKSM_LIB_DIR="$build" cargo install --path "$src/tui" --root "$prefix" --offline --locked 2> /dev/null ||
        XKSM_LIB_DIR="$build" cargo install --path "$src/tui" --root "$prefix" --locked
else
    echo "cargo not found: the terminal interface xksm-tui is not installed"
fi
if [ -n "$port" ]; then
    echo "== record port $port"
    store="${XKSM_HOME:-$HOME/XKS-memories}/.index/agents.xksm"
    mkdir -p "$(dirname "$store")" && chmod 700 "${XKSM_HOME:-$HOME/XKS-memories}" "$(dirname "$store")"
    "$bin" init "$store" > /dev/null
    "$bin" port "$store" "$port"
fi
if [ "$claude_reg" = 1 ] && command -v claude > /dev/null 2>&1; then
    echo "== register the Claude Code plugin"
    claude plugin marketplace add "$prefix/share/xks-memory/claude-code" || true
    claude plugin install xks-memory@xks-memory
elif [ "$claude_reg" = 1 ]; then
    echo "claude CLI not found: register later with"
    echo "  claude plugin marketplace add $prefix/share/xks-memory/claude-code"
    echo "  claude plugin install xks-memory@xks-memory"
fi
if [ "$backfill" = 1 ]; then
    echo "== backfill Claude Code transcripts"
    "$bin" transcripts
fi
echo "== done: application in $prefix/share/xks-memory, memories in $root (private, 700)"
