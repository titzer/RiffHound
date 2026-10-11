#!/bin/bash
# Run lyrbench over the annotated tracks that have lyrics and cached stems.
#   scripts/lyric-bench.sh [lyrbench flags...]
# With --hints, adds the top synced candidate scripts/lyrics_find.py has
# cached for each track (no network: --check).
cd "$(dirname "$0")/.."
hints=0
for a in "$@"; do [ "$a" = --hints ] && hints=1; done
args=()
for map in bench/tracks-anno/*.txt; do
    grep -q 'lyric:' "$map" || continue
    audio=$(ls "${map%.txt}".* | grep -v '\.txt$' | head -1)
    voc=$(external/venv-stems/bin/python scripts/stems_demucs.py "$audio" --check 2>/dev/null \
          | awk -F'\t' '$1 == "stem" && $2 == "vocals" { print $3 }')
    [ -n "$voc" ] || continue
    args+=("$map" "$voc")
    if [ "$hints" = 1 ]; then
        tsv=$(python3 scripts/lyrics_find.py "$audio" --check 2>/dev/null \
              | awk -F'\t' '$1 == "cand" && $5 == "synced" && $11 != "" { print $11; exit }')
        args+=("${tsv:--}")
    fi
done
exec ./lyrbench "$@" "${args[@]}"
