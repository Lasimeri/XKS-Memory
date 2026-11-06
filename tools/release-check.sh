#!/bin/sh
# release-check.sh: prove a release builds and runs from its source alone, on this machine.
# Takes a clean copy (a git clone of a repository, or a release's source tarball), runs
# tools/install.sh from it into a scratch prefix and a scratch memory root (never the real
# ones), then stores a turn, lists it, verifies every hash and opens the terminal interface
# headless. Nothing is uploaded anywhere; nothing outside the scratch directory is touched.
#   tools/release-check.sh [--tag vX.Y.Z] [SOURCE]
#     SOURCE    a repository to clone (path or URL; default: this repository's own directory)
#     --tag T   check out tag T of the clone, or with a github.com URL fetch the tag's tarball
#               (exactly what a release download is: no git history)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
tag=""
src=""
while [ $# -gt 0 ]; do
    case "$1" in
    --tag) tag=$2; shift 2 ;;
    -h | --help) sed -n '2,11p' "$0"; exit 0 ;;
    *) src=$1; shift ;;
    esac
done
[ -n "$src" ] || src=$here
work=$(mktemp -d "${TMPDIR:-/tmp}/xksm-release-check.XXXXXX")
echo "== scratch: $work"
case "$src" in
https://github.com/*)
    if [ -n "$tag" ]; then
        url="${src%.git}/archive/refs/tags/$tag.tar.gz"
        echo "== tarball $url"
        curl -fsSL -o "$work/src.tar.gz" "$url"
        echo "   $(wc -c < "$work/src.tar.gz") bytes, sha256 $(sha256sum "$work/src.tar.gz" | cut -c1-64)"
        mkdir "$work/x"
        tar -xzf "$work/src.tar.gz" -C "$work/x"
        tree=$(find "$work/x" -mindepth 1 -maxdepth 1 -type d | head -n 1)
    else
        git clone -q "$src" "$work/src"
        tree=$work/src
    fi
    ;;
*)
    git clone -q "$src" "$work/src"
    tree=$work/src
    [ -n "$tag" ] && git -C "$tree" checkout -q "$tag"
    ;;
esac
[ -d "$tree/.git" ] && echo "== source: $(git -C "$tree" log --oneline -1)"
export XKSM_HOME="$work/home"
unset XKSM_STORE
t0=$(date +%s)
sh "$tree/tools/install.sh" --prefix "$work/prefix" --no-claude
echo "== install from source: $(($(date +%s) - t0)) s"
bin=$work/prefix/bin
printf 'placeholder release-check turn' | "$bin/xksm" turn --harness release-check --session s1 --role user
"$bin/xksm" sessions release-check
"$bin/xksm" verify
if [ -x "$bin/xksm-tui" ]; then
    printf 'Snap:first\n' > "$work/tui.script"
    (cd "$work" && "$bin/xksm-tui" --script tui.script --snapshot snap "$XKSM_HOME/.index/agents.xksm" > /dev/null)
    grep -q 'placeholder release-check turn' "$work/snap/first.txt" && echo "== xksm-tui shows the turn"
fi
echo "== release check passed; scratch left in $work (delete it when done)"
