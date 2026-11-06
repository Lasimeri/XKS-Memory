#!/bin/sh
# port_words.sh: natural-language word list for tools/port_build --words=FILE, harvested from
# local text (documentation, man pages); nothing is downloaded. Every regular file under the
# given directories is read: *.gz through zcat, other files only if they are text (grep -I).
# Words: lowercase ASCII letters, 3 to 30 long, one per line, sorted, unique.
# usage: tools/port_words.sh OUT DIR...   e.g. tools/port_words.sh words.txt /usr/share/doc /usr/share/man
set -eu
[ $# -ge 2 ] || { echo "usage: $0 OUT DIR..." >&2; exit 2; }
out=$1
shift
export LC_ALL=C
find "$@" -type f -print | while IFS= read -r f; do
    case $f in
    *.gz) zcat -- "$f" 2>/dev/null || true ;;
    *) if grep -Iq . -- "$f" 2>/dev/null; then cat -- "$f"; fi ;;
    esac
done | tr 'A-Z' 'a-z' | tr -c 'a-z\n' '\n' | grep -E '^[a-z]{3,30}$' | sort -u > "$out"
wc -l < "$out" | sed "s|^|$out: |; s|\$| words|"
