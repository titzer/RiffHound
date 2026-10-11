#!/bin/bash
# Run lyrbench over the annotated tracks that have lyrics and cached stems.
#   scripts/lyric-bench.sh [lyrbench flags...]
cd "$(dirname "$0")/.."
args=()
for map in bench/tracks-anno/*.txt; do
    grep -q 'lyric:' "$map" || continue
    audio=$(ls "${map%.txt}".* | grep -v '\.txt$' | head -1)
    voc=$(external/venv-stems/bin/python scripts/stems_demucs.py "$audio" --check 2>/dev/null \
          | awk -F'\t' '$1 == "stem" && $2 == "vocals" { print $3 }')
    [ -n "$voc" ] && args+=("$map" "$voc")
done
exec ./lyrbench "$@" "${args[@]}"
