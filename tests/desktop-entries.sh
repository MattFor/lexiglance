#!/bin/sh
# Lexiglance's menu entries on Linux, through the settings application's --claim-entries and --menu-entry, in a home of
# their own: found under any name and in any data directory, edits made by hand kept.
#   desktop-entries.sh <path to the lexiglance program>
set -u
gui=$1
# APPIMAGE counts only for a program inside APPDIR, as within an AppImage's mount.
appdir=$(cd "$(dirname "$gui")" && pwd -P)
source_dir=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d)
trap 'chmod -R u+w "$root"; rm -rf "$root"' EXIT
fails=0

fail() {
	echo "FAIL ($case): $*" >&2
	fails=$((fails + 1))
}

# A home of its own, with two copies of Lexiglance (AppImages) in it.
fresh() {
	case=$1
	t=$root/$1
	mkdir -p "$t/data/applications" "$t/sys/applications" "$t/home" "$t/old" "$t/new" "$t/run"
	chmod 700 "$t/run"
	a=$t/old/lexiglance-x86_64.AppImage
	b=$t/new/Lexiglance-x86_64_9f8e.AppImage
	: >"$a"
	: >"$b"
	chmod +x "$a" "$b"
	user=$t/data/applications/io.github.mattfor.lexiglance.desktop
}

# run <copy> <arguments>: the program, as that copy.
run() {
	copy=$1
	shift
	env -i PATH="$PATH" HOME="$t/home" XDG_DATA_HOME="$t/data" XDG_DATA_DIRS="$t/sys" XDG_CONFIG_HOME="$t/config" \
		XDG_STATE_HOME="$t/state" XDG_CACHE_HOME="$t/cache" XDG_RUNTIME_DIR="$t/run" QT_QPA_PLATFORM=offscreen \
		APPDIR="$appdir" APPIMAGE="$copy" "$gui" "$@" >"$t/out" 2>&1 || fail "lexiglance $* failed: $(cat "$t/out")"
}

has() {
	grep -qxF -- "$2" "$1" || fail "$1 lacks the line: $2"
}

lacks() {
	if grep -qxF -- "$2" "$1"; then fail "$1 has the line: $2"; fi
}

absent() {
	if [ -e "$1" ]; then fail "$1 exists"; fi
}

# The marker holds the checksum of the rest of the entry: one the program wrote and nobody changed.
sealed() {
	marker=$(sed -n 's/^X-Lexiglance-Generated=//p' "$1")
	sum=$(grep -v '^X-Lexiglance-Generated=' "$1" | sha256sum | cut -c1-16)
	[ "$marker" = "$sum" ] || fail "$1 is marked '$marker', its checksum is $sum"
}

fresh first-start
run "$a" --claim-entries
has "$user" "Exec=\"$a\""
has "$user" "TryExec=$a"
has "$user" "Categories=Utility;TextTools;Dictionary;"
sealed "$user"

# Untouched: written afresh for the copy that claims it.
run "$b" --claim-entries
has "$user" "Exec=\"$b\" --page search"
sealed "$user"
cp "$user" "$root/unedited"

fresh edited-by-hand
cp "$root/unedited" "$user"
sed -i -e "s|$root/first-start|$t|g" -e 's/^Categories=.*/Categories=Education;/' -e 's/^Name=.*/Name=My dictionary/' "$user"
run "$a" --claim-entries
has "$user" "Exec=\"$a\""
has "$user" "Exec=\"$a\" --page overview"
has "$user" "TryExec=$a"
has "$user" "Categories=Education;"
has "$user" "Name=My dictionary"

fresh written-by-1.3.9
run "$a" --claim-entries
sed -i 's/^X-Lexiglance-Generated=.*/X-Lexiglance-Generated=true/' "$user"
cp "$user" "$root/1.3.9"
run "$b" --claim-entries
has "$user" "Exec=\"$b\""
sealed "$user"
# 1.3.8 filed it under Education: unedited, so it moves.
sed -e "s|$b|$a|g" -e 's/^Categories=.*/Categories=Education;Languages;/' "$root/1.3.9" >"$user"
run "$b" --claim-entries
has "$user" "Categories=Utility;TextTools;Dictionary;"
sealed "$user"
# Changed by hand back then: kept, and the marker stays as it was.
sed -e "s|$b|$a|g" -e 's/^Categories=.*/Categories=Office;/' "$root/1.3.9" >"$user"
run "$b" --claim-entries
has "$user" "Exec=\"$b\""
has "$user" "Categories=Office;"
has "$user" "X-Lexiglance-Generated=true"

fresh other-name-gone-program
other=$t/data/applications/appimagekit_1a2b-Lexiglance.desktop
cat >"$other" <<EOF
[Desktop Entry]
Type=Application
Name=Lexiglance (AppImage)
Exec=env QT_SCALE_FACTOR=1.5 "/gone/Lexiglance-x86_64_1a2b.AppImage" %U
TryExec=/gone/Lexiglance-x86_64_1a2b.AppImage
Categories=Office;
EOF
run "$a" --claim-entries
has "$other" "Exec=env QT_SCALE_FACTOR=1.5 \"$a\" %U"
has "$other" "TryExec=$a"
has "$other" "Categories=Office;"
absent "$user"

fresh other-name-other-copy
mkdir -p "$t/data/applications/mine"
other=$t/data/applications/mine/dictionary.desktop
printf '[Desktop Entry]\nType=Application\nName=Dictionary\nExec="%s" --tray\n' "$b" >"$other"
cp "$other" "$t/before"
run "$a" --claim-entries
cmp -s "$other" "$t/before" || fail "an entry starting another copy that is there was changed"
absent "$user"

fresh not-lexiglance
notes=$t/data/applications/notes.desktop
printf '[Desktop Entry]\nType=Application\nName=Notes\nExec=nvim /home/me/lexiglance.txt\n' >"$notes"
cp "$notes" "$t/before"
run "$a" --claim-entries
cmp -s "$notes" "$t/before" || fail "an entry that is not Lexiglance's was changed"
has "$user" "Exec=\"$a\""

fresh linked
mkdir -p "$t/elsewhere"
printf '[Desktop Entry]\nType=Application\nName=Lexiglance\nExec=/gone/lexiglance\n' >"$t/elsewhere/l.desktop"
ln -s "$t/elsewhere/l.desktop" "$t/data/applications/linked-lexiglance.desktop"
run "$a" --claim-entries
[ -L "$t/data/applications/linked-lexiglance.desktop" ] || fail "the link was replaced"
has "$t/elsewhere/l.desktop" "Exec=/gone/lexiglance"
absent "$user"

fresh package
cp "$source_dir/data/linux/io.github.mattfor.lexiglance.desktop" "$t/sys/applications/"
chmod -R a-w "$t/sys"
run "$a" --claim-entries
absent "$user"
run "$a" --menu-entry off
has "$user" "Hidden=true"
run "$a" --menu-entry on
absent "$user"

fresh own-entry-hidden-in-place
mine=$t/data/applications/my-lexiglance.desktop
printf '[Desktop Entry]\nType=Application\nName=Mine\nExec="%s"\n' "$a" >"$mine"
cp "$mine" "$t/before"
run "$a" --menu-entry off
has "$mine" "NoDisplay=true"
has "$mine" "Name=Mine"
absent "$user"
run "$a" --menu-entry on
cmp -s "$mine" "$t/before" || fail "showing it again did not undo the hiding"
absent "$user"

fresh generated-edited-then-hidden
run "$a" --claim-entries
sed -i 's/^Name=.*/Name=Edited/' "$user"
run "$a" --menu-entry off
has "$user" "NoDisplay=true"
run "$a" --menu-entry on
lacks "$user" "NoDisplay=true"
has "$user" "Name=Edited"
run "$a" --menu-entry off
run "$a" --menu-entry on
has "$user" "Name=Edited"

fresh generated-taken-out
run "$a" --claim-entries
run "$a" --menu-entry off
absent "$user"
run "$a" --claim-entries
absent "$user"
run "$a" --menu-entry on
has "$user" "Exec=\"$a\""

[ "$fails" -eq 0 ] && echo "desktop entries: all passed"
[ "$fails" -eq 0 ]
