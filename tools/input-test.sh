#!/usr/bin/env bash
# Tries the keys and the wheel on a daemon, as fast as a hand at its most impatient, without touching the desktop: the
# daemon runs in a display of its own (Xvfb) over a page of Japanese, xdotool holds the trigger, presses the sentence
# key and turns the wheel, and the daemon's own log says what came of it. Every press of the sentence key has to
# count, pressed again before the translation is made too, and text chosen with the wheel has to stay chosen while the
# pointer wobbles.
#   bash tools/input-test.sh <lexiglanced>
# Reads with what is installed for this user, none of which it writes to: PaddleOCR (Scanning -> Download PaddleOCR),
# the Japanese translation model (Translation -> Download) and a Japanese dictionary. Needs Xvfb, xdotool, ImageMagick
# and fontconfig. Leaves its log and screenshots behind when a check fails (KEEP=1: always).
set -euo pipefail

daemon="${1:?usage: input-test.sh <lexiglanced>}"
for tool in Xvfb xdotool magick display import fc-match; do
    command -v "$tool" >/dev/null || {
        echo "$tool is needed" >&2
        exit 2
    }
done
data="${XDG_DATA_HOME:-$HOME/.local/share}/lexiglance"
for needed in ocr/paddle/det.onnx ocr/runtime translation/opus-mt-ja-en/tokenizer.json 'dictionaries/*.lgd'; do
    compgen -G "$data/$needed" >/dev/null || {
        echo "$data/$needed is needed: PaddleOCR, the Japanese translation model and a Japanese dictionary" >&2
        exit 2
    }
done

work="$(mktemp -d)"
log="$work/daemon.log"
mkdir -p "$work/home/config" "$work/home/data/dictionaries"
ln -s "$data/ocr" "$data/translation" "$work/home/data/"
ln -s "$data"/dictionaries/*.lgd "$work/home/data/dictionaries/"
cat >"$work/home/config/config.json" <<'EOF'
{
  "language": "ja",
  "statistics": false,
  "scan": { "trigger": ["Super", "Alt_L"], "accessibility": false, "selection": "off", "ocr": "always", "ocr_engine": "paddle" },
  "translation": { "enabled": true, "sentence_key": "Shift_L" },
  "audio": { "enabled": false },
  "anki": { "enabled": false }
}
EOF
# Three sentences in 30 px letters from x = 80; a line's letters are 28 px above the y given.
magick -size 1600x900 xc:'#f5f5f5' -font "$(fc-match -f '%{file}' ':lang=ja')" -pointsize 30 -fill '#141414' \
    -annotate +80+150 '魔王を倒すために、勇者は仲間と長い旅に出ることにした。' \
    -annotate +80+330 '図書館で借りた本を明日までに返さなければなりません。' \
    -annotate +80+510 '彼女は毎朝コーヒーを飲みながら新聞を読む。' "$work/page.png"

Xvfb -displayfd 3 -screen 0 1600x900x24 -nolisten tcp 3>"$work/display" >"$work/xvfb.log" 2>&1 &
started=($!)
failures=0
# shellcheck disable=SC2329 # called by the trap
finish() {
    kill "${started[@]}" 2>/dev/null || true
    if [ "$failures" -eq 0 ] && [ -z "${KEEP:-}" ]; then
        rm -rf "$work"
    else
        echo "the log and screenshots are in $work"
    fi
}
trap finish EXIT
for _ in $(seq 50); do
    [ -s "$work/display" ] && break
    sleep 0.1
done
DISPLAY=":$(cat "$work/display")"
export DISPLAY LEXIGLANCE_HOME="$work/home"
# Nothing of the session this is started from: its accessibility bus would be read instead of the page.
unset DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS WAYLAND_DISPLAY

display -geometry +0+0 "$work/page.png" >/dev/null 2>&1 &
started+=($!)
"$daemon" --verbose >>"$log" 2>&1 &
started+=($!)
for _ in $(seq 300); do
    grep -q 'text capture:' "$log" && break
    sleep 0.1
done
if ! grep -q 'text capture:.*OCR (' "$log"; then
    echo "the daemon did not come up reading the screen:" >&2
    tail -n 5 "$log" >&2
    failures=1
    exit 1
fi
sleep 1

# A step of the test, marked in the log, with a screenshot of how the step before it ended.
step() {
    import -window root "$work/before-$1.png" 2>/dev/null || true
    echo "#### $1" >>"$log"
}
# How many lines of the log between a step's mark and the next one match.
logged() {
    awk -v from="#### $1" -v pattern="$2" '$0 == from { on = 1; next } /^####/ { on = 0 } on && $0 ~ pattern { n++ } END { print n + 0 }' "$log"
}
check() {
    if test "$2" "$3" "$4"; then
        echo "ok    $1"
    else
        echo "FAIL  $1 ($2 $3 $4 does not hold)"
        failures=$((failures + 1))
    fi
}
tap() {
    xdotool keydown Shift_L
    sleep 0.03
    xdotool keyup Shift_L
    sleep "${1:-0.05}"
}

xdotool mousemove 365 138
step trigger
xdotool keydown Super_L keydown Alt_L
sleep 3
step translate
tap 6
step burst
for _ in 1 2 3 4 5; do tap; done
sleep 2
# Another sentence, pressed twice before its translation can be done: on, and off again. Left of the popup, which
# hangs over this line: the pointer on the popup looks nothing up.
xdotool mousemove 200 318
sleep 2
step early
tap 0.1
tap 5
xdotool mousemove 365 138
sleep 2
step wheel
for notch in 1 2 3 4 5 6; do
    xdotool click 4
    xdotool mousemove_relative -- $(((notch % 2) * 10 - 5)) $(((notch % 3) * 3 - 3))
    sleep 0.03
done
sleep 2
step chosen
tap 6
step release
xdotool keyup Alt_L keyup Super_L
sleep 1
step end

check 'the word under the pointer is looked up' "$(logged trigger 'lookup: «')" -ge 1
check 'the sentence key translates the sentence' "$(logged translate 'translation: [0-9]+ characters')" -ge 1
check 'five quick presses turn it off, on, off, on, off' "$(logged burst 'turned off with the sentence key')" -eq 3
check 'pressed again before the translation is made, it is turned off' "$(logged early 'turned off with the sentence key')" -eq 1
check 'the wheel chooses more text' "$(logged wheel 'lookup: [0-9]+ terms')" -ge 1
check 'the pointer wobbling on the wheel does not look the word up afresh' "$(logged wheel 'capture: [0-9]+ characters')" -eq 0
check 'the sentence key translates the text the wheel chose' "$(logged chosen 'translation: [0-9]+ characters')" -ge 1
check 'letting go of the trigger closes the popup' "$(logged release 'popup closed')" -ge 1
exit "$((failures > 0))"
