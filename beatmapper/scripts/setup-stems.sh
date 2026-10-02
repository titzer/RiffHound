#!/usr/bin/env bash
# Set up the Demucs stem-separation environment beatmapper calls
# (Stem Layers tool > "Separate stems", or scripts/stems_demucs.py directly).
#
#   scripts/setup-stems.sh [--force] [--check]
#
# Installs demucs + torch into external/venv-stems (git-ignored) with Python
# 3.12 via uv when available, else the first python3.10-3.13 on PATH.  The
# htdemucs_6s weights (~80 MB) are fetched by torch on the first run and kept
# in ~/.cache/torch/hub/checkpoints/.  Per-track stems are cached in
# ~/.cache/beatmapper/stems/.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
VENV="$ROOT/external/venv-stems"
FORCE=0; CHECK_ONLY=0
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        --check) CHECK_ONLY=1 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $a (try --help)" >&2; exit 2 ;;
    esac
done
info() { printf '\033[1m== %s\033[0m\n' "$*"; }
ok()   { printf '   \033[32mOK\033[0m  %s\n' "$*"; }
bad()  { printf '   \033[31mFAIL\033[0m %s\n' "$*"; }

verify() {
    [ -x "$VENV/bin/python" ] && \
    "$VENV/bin/python" -c "import torch, demucs.api, soundfile" 2>/dev/null
}

info "Demucs -> $VENV"
[ "$FORCE" = 1 ] && rm -rf "$VENV"
if verify; then ok "already installed and importable"; exit 0; fi
[ "$CHECK_ONLY" = 1 ] && { bad "not installed (run without --check to install)"; exit 1; }
mkdir -p "$ROOT/external"
command -v ffmpeg >/dev/null 2>&1 || echo "note: ffmpeg not on PATH -- mp3/m4a decoding needs it (brew install ffmpeg)"

if command -v uv >/dev/null 2>&1; then
    uv venv -q --python 3.12 "$VENV" && \
    uv pip install -q --python "$VENV/bin/python" demucs torch torchaudio soundfile \
        || { bad "uv install failed"; exit 1; }
else
    py=""
    for p in python3.12 python3.11 python3.13 python3.10; do
        command -v "$p" >/dev/null 2>&1 && { py="$p"; break; }
    done
    [ -n "$py" ] || { bad "no python3.10-3.13 found (or install uv)"; exit 1; }
    "$py" -m venv "$VENV" && "$VENV/bin/pip" install -q --upgrade pip && \
    "$VENV/bin/pip" install -q demucs torch torchaudio soundfile \
        || { bad "pip install failed"; exit 1; }
fi
verify && ok "installed" || { bad "installed but import fails"; exit 1; }
echo "  App: Stem Layers tool > Separate stems (first run downloads the model weights)"
