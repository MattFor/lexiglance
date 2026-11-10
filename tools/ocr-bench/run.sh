#!/usr/bin/env bash
# Measures how well a build reads text off the screen: draws lines of Japanese, Russian, Korean and Greek in some fifty
# fonts (pixel, handwritten, brush and display faces among them), at sizes from 10 to 96 px, outlined, shadowed, faint
# and over a busy scene, has the build's own screen reading (ocrbench) read every line, and says how much of it came
# out wrong. For telling whether a change to the OCR helps: run it before and after.
#   bash tools/ocr-bench/run.sh <build directory> <label> [earlier label] [ocrbench options]
# The build is one configured with -DLEXIGLANCE_BUILD_TOOLS=ON. The results go to <label>.tsv, and are shown beside
# those of the earlier label when one is given (summary.py shows more: --fonts, --worse). Options for ocrbench:
# --lookup, --beside, --languages ja,ru (see bench.cpp).
#   QUICK=1    a fifth of the pictures, for trying a change (a minute or two instead of ten)
#   JOBS=8     how many read at once
#   OCR_BENCH_DIR=build/ocr-bench    where the fonts, the pictures, the models and the results are kept
# The first run downloads the fonts (100 MB) and draws the pictures (160 MB). It reads with the PaddleOCR installed
# for this user (Scanning -> Download PaddleOCR) and fetches the recognisers of the languages that are not, since
# with all of them loaded they also have to be told apart. Needs Python with Pillow and NumPy, fontconfig and curl.
set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${1:?usage: run.sh <build directory> <label> [earlier label] [ocrbench options]}"
label="${2:?usage: run.sh <build directory> <label> [earlier label] [ocrbench options]}"
shift 2
earlier=""
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
    earlier="$1"
    shift
fi
work="${OCR_BENCH_DIR:-$repository/build/ocr-bench}"
jobs="${JOBS:-8}"
driver="$build/tools/ocrbench"
if [ ! -x "$driver" ]; then
    echo "no $driver: configure the build with -DLEXIGLANCE_BUILD_TOOLS=ON" >&2
    exit 2
fi

mkdir -p "$work/results"
if [ ! -s "$work/pictures/truth.tsv" ]; then
    bash "$repository/tools/ocr-bench/fonts.sh" "$work/fonts"
    python3 "$repository/tools/ocr-bench/gen.py" "$work/fonts" "$work/pictures"
fi

# A home of its own with the installed models and the missing recognisers, so nothing of the user's is written to.
installed="${XDG_DATA_HOME:-$HOME/.local/share}/lexiglance/ocr"
models="$work/home/data/ocr"
if [ ! -s "$installed/paddle/det.onnx" ] || [ ! -d "$installed/runtime" ]; then
    echo "PaddleOCR is not installed in $installed: Scanning -> Download PaddleOCR in the settings application" >&2
    exit 2
fi
mkdir -p "$models/paddle"
ln -sfn "$installed/runtime" "$models/runtime"
for model in "$installed"/paddle/*.onnx; do
    ln -sfn "$model" "$models/paddle/"
done
# What is not installed comes from where the settings application gets it (apps/gui/OcrInstall.cpp): the default
# recogniser, and those the languages name.
source="$repository/apps/gui/OcrInstall.cpp"
release="$(sed -n 's/.*paddle_release *= *"\(.*\)";/\1/p' "$source")"
fetch() {
    if [ ! -s "$models/paddle/$1" ]; then
        echo "downloading $1"
        curl -fsSL --retry 3 -o "$models/paddle/$1" "$2" || {
            rm -f "$models/paddle/$1"
            echo "not downloaded: $2" >&2
        }
    fi
}
fetch rec.onnx "$(sed -n 's/.*paddle_models *= *"\(.*\)";/\1/p' "$source")$(sed -n 's/.*"\(rec\/[^"]*\.onnx\)".*/\1/p' "$source")"
sed -n 's/.*"paddle": *"\(.*\)".*/\1/p' "$repository"/data/languages/*.json | sort -u | while read -r recogniser; do
    fetch "rec-${recogniser##*/}" "$release$recogniser"
done

truth="$work/pictures/truth.tsv"
if [ -n "${QUICK:-}" ]; then
    truth="$work/pictures/quick.tsv"
fi
started=$(date +%s)
readers=()
for ((shard = 0; shard < jobs; ++shard)); do
    LEXIGLANCE_HOME="$work/home" "$driver" "$truth" "$work/pictures" --shard "$shard/$jobs" "$@" \
        >"$work/results/$label.$shard.part" 2>"$work/results/$label.$shard.log" &
    readers+=($!)
done
failed=0
for reader in "${readers[@]}"; do
    wait "$reader" || failed=1
done
if [ "$failed" -ne 0 ]; then
    cat "$work/results/$label".*.log >&2
    rm -f "$work/results/$label".*.part
    exit 1
fi
sort "$work/results/$label".*.part >"$work/results/$label.tsv"
rm -f "$work/results/$label".*.part "$work/results/$label".*.log
echo "$label: $(wc -l <"$work/results/$label.tsv") lines in $(($(date +%s) - started)) s, in $work/results/$label.tsv"
if [ -n "$earlier" ]; then
    python3 "$repository/tools/ocr-bench/summary.py" "$work/results/$label.tsv" "$work/results/$earlier.tsv"
else
    python3 "$repository/tools/ocr-bench/summary.py" "$work/results/$label.tsv"
fi
