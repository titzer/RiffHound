#!/usr/bin/env bash
# Set up the external learned chord-recognition models beatmapper can call
# (Complete Track > Chord settings > "External model", or
#  bmbench --set chord_external=1).
#
#   scripts/setup-chord-models.sh [madmom|btc|all] [--force] [--check]
#
#     all (default)  set up both models
#     madmom         madmom CNN+CRF   -> external/venv        (the default model)
#     btc            BTC large-vocab  -> external/venv-btc    (7th/sus chords;
#                    select with BM_CHORD_CMD, see below)
#     --force        wipe and rebuild the chosen environments
#     --check        only verify, install nothing
#
# Everything lands under beatmapper/external/ (git-ignored); per-track results
# are cached in ~/.cache/beatmapper/chords/ so each model runs once per track.
#
# Versions are pinned to a configuration verified on macOS arm64 (2026-09):
#   madmom from git @27f032e (the PyPI 0.16.1 release does not build on
#   modern numpy), any Python >= 3.10;
#   BTC @2682317 with torch (CPU), librosa 0.9.2, numpy 1.x, Python 3.9-3.11
#   (newer librosa changed APIs the 2019 code relies on).
#
# To use BTC instead of madmom:
#   export BM_CHORD_CMD="$PWD/external/venv-btc/bin/python $PWD/scripts/chords_btc.py"
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
EXT="$ROOT/external"
MADMOM_VENV="$EXT/venv"
BTC_VENV="$EXT/venv-btc"
BTC_REPO="$EXT/BTC-ISMIR19"
MADMOM_PIN="27f032e8947204902c675e5e341a3faf5dc86dae"
BTC_PIN="2682317"

WANT_MADMOM=1
WANT_BTC=1
FORCE=0
CHECK_ONLY=0
for a in "$@"; do
    case "$a" in
        madmom)  WANT_BTC=0 ;;
        btc)     WANT_MADMOM=0 ;;
        all)     ;;
        --force) FORCE=1 ;;
        --check) CHECK_ONLY=1 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $a (try --help)" >&2; exit 2 ;;
    esac
done

info() { printf '\033[1m== %s\033[0m\n' "$*"; }
ok()   { printf '   \033[32mOK\033[0m  %s\n' "$*"; }
bad()  { printf '   \033[31mFAIL\033[0m %s\n' "$*"; }

# Pick the first python whose version test passes.
find_python() {  # find_python "test-expr" candidates...
    local test_expr="$1"; shift
    for p in "$@"; do
        local bin
        bin="$(command -v "$p" 2>/dev/null)" || continue
        "$bin" -c "import sys; sys.exit(0 if ($test_expr) else 1)" 2>/dev/null && { echo "$bin"; return 0; }
    done
    return 1
}

verify_madmom() {
    [ -x "$MADMOM_VENV/bin/python" ] && \
    "$MADMOM_VENV/bin/python" -c \
        "from madmom.features.chords import CNNChordFeatureProcessor, CRFChordRecognitionProcessor" \
        2>/dev/null
}

verify_btc() {
    [ -x "$BTC_VENV/bin/python" ] && \
    [ -f "$BTC_REPO/test/btc_model_large_voca.pt" ] && \
    "$BTC_VENV/bin/python" - <<PY 2>/dev/null
import sys
sys.path.insert(0, "$BTC_REPO")
import torch, librosa, yaml, mir_eval
from btc_model import BTC_model
PY
}

setup_madmom() {
    info "madmom CNN+CRF -> $MADMOM_VENV"
    if [ "$FORCE" = 1 ]; then rm -rf "$MADMOM_VENV"; fi
    if verify_madmom; then ok "already installed and importable"; return 0; fi
    [ "$CHECK_ONLY" = 1 ] && { bad "not installed (run without --check to install)"; return 1; }

    local py
    py="$(find_python "sys.version_info >= (3, 10)" \
          "${PYTHON_MADMOM:-}" python3.14 python3.13 python3.12 python3.11 python3.10 \
          /opt/homebrew/bin/python3.14 /opt/homebrew/bin/python3 python3)" \
        || { bad "no Python >= 3.10 found (set PYTHON_MADMOM=/path/to/python)"; return 1; }
    echo "   using $py ($("$py" -c 'import sys; print(sys.version.split()[0])'))"

    "$py" -m venv "$MADMOM_VENV" || { bad "venv creation failed"; return 1; }
    "$MADMOM_VENV/bin/pip" install -q --upgrade pip &&
    "$MADMOM_VENV/bin/pip" install -q cython numpy scipy &&
    "$MADMOM_VENV/bin/pip" install -q "git+https://github.com/CPJKU/madmom@$MADMOM_PIN" \
        || { bad "pip install failed (madmom needs git; PyPI 0.16.1 does not build)"; return 1; }
    verify_madmom && ok "installed" || { bad "installed but import fails"; return 1; }
}

setup_btc() {
    info "BTC large-vocabulary -> $BTC_VENV"
    if [ "$FORCE" = 1 ]; then rm -rf "$BTC_VENV"; fi
    if verify_btc; then ok "already installed and importable"; return 0; fi
    [ "$CHECK_ONLY" = 1 ] && { bad "not installed (run without --check to install)"; return 1; }

    if [ ! -d "$BTC_REPO/.git" ]; then
        git clone -q https://github.com/jayg996/BTC-ISMIR19 "$BTC_REPO" \
            || { bad "git clone failed"; return 1; }
    fi
    git -C "$BTC_REPO" checkout -q "$BTC_PIN" 2>/dev/null || true
    # np.float was removed in numpy >= 1.24; the 2019 code still uses it.
    if grep -q "astype(np.float)" "$BTC_REPO/utils/transformer_modules.py" 2>/dev/null; then
        python3 - "$BTC_REPO/utils/transformer_modules.py" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
open(p, "w").write(s.replace("astype(np.float)", "astype(float)"))
PY
        echo "   patched utils/transformer_modules.py (np.float -> float)"
    fi
    [ -f "$BTC_REPO/test/btc_model_large_voca.pt" ] \
        || { bad "pretrained weights missing from the clone"; return 1; }

    local py
    py="$(find_python "(3, 9) <= sys.version_info < (3, 12)" \
          "${PYTHON_BTC:-}" python3.11 python3.10 python3.9 /usr/bin/python3)" \
        || { bad "no Python 3.9-3.11 found (librosa<0.10 needs it; set PYTHON_BTC=...)"; return 1; }
    echo "   using $py ($("$py" -c 'import sys; print(sys.version.split()[0])'))"

    "$py" -m venv "$BTC_VENV" || { bad "venv creation failed"; return 1; }
    "$BTC_VENV/bin/pip" install -q --upgrade pip &&
    "$BTC_VENV/bin/pip" install -q torch "librosa<0.10" "numpy<2" mir_eval pyyaml \
        || { bad "pip install failed"; return 1; }
    verify_btc && ok "installed" || { bad "installed but import fails"; return 1; }
}

mkdir -p "$EXT"
command -v ffmpeg >/dev/null 2>&1 \
    || echo "note: ffmpeg not on PATH -- mp3/m4a decoding needs it (brew install ffmpeg)"

status=0
[ "$WANT_MADMOM" = 1 ] && { setup_madmom || status=1; }
[ "$WANT_BTC" = 1 ]    && { setup_btc    || status=1; }

echo
if [ "$status" = 0 ]; then
    info "ready"
    echo "  App:   Complete Track > Chord settings > 'External model (madmom)'"
    echo "  Bench: ./bmbench <track> complete --set chord_external=1 ..."
    [ "$WANT_BTC" = 1 ] && {
        echo "  BTC:   BM_CHORD_CMD=\"$BTC_VENV/bin/python $SCRIPT_DIR/chords_btc.py\" ./bmbench ..."
    }
    echo "  First run per track pays the model (~5-30 s); after that it is cached"
    echo "  in ~/.cache/beatmapper/chords/."
else
    info "some components failed -- see FAIL lines above"
fi
exit $status
