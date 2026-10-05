#!/usr/bin/env bash
# Builds Lexiglance and puts it where this machine actually runs it from, so a change can be tried on your own desktop:
# the copy the login autostart (or a systemd or runit service) starts. A build tree that runs in place (build/dev, say)
# is rebuilt where it is; an installed copy is installed over. The daemon, and the settings application if it was open,
# start again on the new build, and the autostart, the menu entry and the links in ~/.local/bin start it from then on.
# Copies elsewhere on PATH that would shadow it are pointed out.
#   bash .github/scripts/dev-install.sh [preset] [prefix]
# With a preset or prefix given, that is used instead of what the autostart starts. When the autostart starts a release
# AppImage (Install release, or an AppImage started since, takes it over) or nothing, the build tree this script put in
# place last time is rebuilt and put back; without one, the release preset is installed into ~/.local.
set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repository"

# The build tree this script put in place last, kept for when a release has taken the autostart over since.
remembered="$repository/build/.dev-install"

# The program something starts at login (a daemon executable, or an AppImage): the XDG autostart entry, else a systemd
# user unit or a runit service.
started_from() {
    local entry="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/lexiglance-daemon.desktop"
    if [ -f "$entry" ]; then
        # Exec="/a path/program" --argument, or Exec=/path/program --argument.
        sed -n -e 's/^Exec="\([^"]*\)".*$/\1/p' -e 's/^Exec=\([^" ][^ ]*\).*$/\1/p' "$entry" | head -n 1
        return
    fi
    if systemctl --user cat lexiglanced.service >/dev/null 2>&1; then
        systemctl --user show -p ExecStart --value lexiglanced.service 2>/dev/null | sed -n 's/.*path=\([^ ;]*\).*/\1/p' | head -n 1
        return
    fi
    for run in "${XDG_CONFIG_HOME:-$HOME/.config}/service/lexiglanced/run" /etc/sv/lexiglanced/run; do
        if [ -f "$run" ]; then
            grep -o '[^ "]*lexiglanced' "$run" | head -n 1
            return
        fi
    done
}

# This user's processes running the program `name`, found by their executable: builds before 1.3.0 renamed their main
# thread, and with it the process ("daemon", "settings"), so pgrep -x misses them. A rebuilt one reads "(deleted)".
pids_of() {
    local link target pid
    for link in /proc/[0-9]*/exe; do
        target="$(readlink "$link" 2>/dev/null)" || continue
        target="${target% (deleted)}"
        if [ "${target##*/}" = "$1" ]; then
            pid="${link#/proc/}"
            echo "${pid%/exe}"
        fi
    done
}

# Ends every copy of `name` (asked first, killed after two seconds); true when one was running.
stop_all() {
    local pids
    pids="$(pids_of "$1")"
    [ -n "$pids" ] || return 1
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$(pids_of "$1")" ] || return 0
        sleep 0.2
    done
    # shellcheck disable=SC2046
    kill -9 $(pids_of "$1") 2>/dev/null || true
    return 0
}

preset="${1:-}"
prefix="${2:-}"
build=""
in_place=false
autostarted=false
if [ -z "$preset" ] && [ -z "$prefix" ]; then
    target="$(started_from || true)"
    [ -n "$target" ] && autostarted=true
    case "$target" in
        "$repository"/build/*/apps/daemon/lexiglanced)
            build="${target%/apps/daemon/lexiglanced}"
            in_place=true
            echo "the autostart runs the build tree $build: rebuilding it in place"
            ;;
        */bin/lexiglanced)
            prefix="${target%/bin/lexiglanced}"
            echo "the autostart runs $target: installing into $prefix"
            ;;
        *.AppImage | *.appimage | "")
            last="$(cat "$remembered" 2>/dev/null || true)"
            if [ -n "$last" ] && [ -f "$last/CMakeCache.txt" ]; then
                build="$last"
                in_place=true
                echo "the autostart runs ${target:-nothing}: rebuilding $build (put in place last time), which runs instead"
            else
                echo "the autostart runs ${target:-nothing}: installing into ~/.local"
            fi
            ;;
        *)
            echo "the autostart runs $target, which is neither a build tree here nor an installed prefix" >&2
            echo "pass a preset and prefix: bash $0 release <prefix>" >&2
            exit 1
            ;;
    esac
fi
preset="${preset:-release}"
prefix="${prefix:-$HOME/.local}"

if $in_place; then
    cmake --build "$build"
    daemon="$build/apps/daemon/lexiglanced"
    gui="$build/apps/gui/lexiglance"
    ctl="$build/apps/ctl/lexiglancectl"
else
    build="$repository/build/$preset"
    [ -f "$build/CMakeCache.txt" ] || cmake --preset "$preset"
    cmake --build --preset "$preset"
    daemon="$prefix/bin/lexiglanced"
    gui="$prefix/bin/lexiglance"
    ctl="$prefix/bin/lexiglancectl"
fi

# Whichever supervisor has the daemon, so the same one brings it back.
service=""
if systemctl --user is-active --quiet lexiglanced.service 2>/dev/null; then
    service="systemd"
elif command -v sv >/dev/null 2>&1 && sv status lexiglanced >/dev/null 2>&1; then
    service="runit"
fi
daemon_ran=false
gui_ran=false
[ -n "$(pids_of lexiglanced)" ] && daemon_ran=true
[ -n "$(pids_of lexiglance)" ] && gui_ran=true

# A build tree runs in place: the build above already replaced its programs (running ones keep their old files).
if ! $in_place; then
    case "$service" in
        systemd)
            echo "stopping lexiglanced.service..."
            systemctl --user stop lexiglanced.service
            ;;
        runit)
            echo "stopping lexiglanced (runit)..."
            sv stop lexiglanced
            ;;
        *) if stop_all lexiglanced; then echo "stopped lexiglanced"; fi ;;
    esac
    stop_all lexiglance || true
    echo "installing into $prefix..."
    if [ -w "$prefix" ] || { [ ! -e "$prefix" ] && [ -w "$(dirname "$prefix")" ]; }; then
        cmake --install "$build" --prefix "$prefix"
    else
        sudo cmake --install "$build" --prefix "$prefix"
    fi
fi

# The name's program in the copy just built or installed.
program_for() {
    case "$1" in
        lexiglance) echo "$gui" ;;
        lexiglanced) echo "$daemon" ;;
        *) echo "$ctl" ;;
    esac
}

# The autostart and the menu entry start this copy from now on (a release installed since may have taken them), and so
# do the links of the user's in ~/.local/bin (links only: a copied program is pointed out below instead).
"$gui" --claim-entries || echo "note: the autostart and the menu entry could not be pointed at $gui" >&2
for name in lexiglance lexiglanced lexiglancectl; do
    link="$HOME/.local/bin/$name"
    wanted="$(program_for "$name")"
    if [ -L "$link" ] && [ "$(readlink -f "$link")" != "$(readlink -f "$wanted")" ]; then
        ln -sfn "$wanted" "$link"
        echo "$link now links to $wanted"
    fi
done
if $in_place; then
    echo "$build" >"$remembered"
fi

# The new daemon replaces the running one (or starts); the settings application comes back in the tray if it was open.
case "$service" in
    systemd)
        systemctl --user daemon-reload
        systemctl --user restart lexiglanced.service
        echo "daemon restarted (systemd)."
        ;;
    runit)
        sv restart lexiglanced
        echo "daemon restarted (runit)."
        ;;
    *)
        if $daemon_ran || $autostarted; then
            setsid "$daemon" --replace >/dev/null 2>&1 </dev/null &
            echo "daemon started: $daemon"
        fi
        ;;
esac
if $gui_ran; then
    stop_all lexiglance || true
    setsid "$gui" --tray >/dev/null 2>&1 </dev/null &
    echo "settings application started again in the tray: $gui"
fi

# Other copies earlier on PATH would be run instead of this one from a terminal or the menu.
for name in lexiglance lexiglanced lexiglancectl; do
    found="$(command -v "$name" 2>/dev/null || true)"
    wanted="$(program_for "$name")"
    if [ -n "$found" ] && [ "$(readlink -f "$found")" != "$(readlink -f "$wanted")" ]; then
        echo "note: $name on PATH is $found, not $wanted; remove that copy, or link it here: ln -sf \"$wanted\" ~/.local/bin/$name" >&2
    fi
done
