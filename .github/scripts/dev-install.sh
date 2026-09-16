#!/usr/bin/env bash
# Builds Lexiglance and puts it over the copy installed on this machine, so a change can be tried on your own desktop.
# The daemon holds its executable open while it runs, so it is stopped first and started again afterwards; a daemon
# under systemd, runit, OpenRC, dinit or s6 is left to its own supervisor to restart.
#   bash .github/scripts/dev-install.sh [preset] [prefix]
# Defaults to the release preset and /usr/local, and uses sudo only when the prefix is not writable.
set -euo pipefail

preset="${1:-release}"
prefix="${2:-/usr/local}"
repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$repository/build/$preset"

cd "$repository"
[ -f "$build/CMakeCache.txt" ] || cmake --preset "$preset"
cmake --build --preset "$preset"

# Whichever supervisor has it, so the same one brings it back.
service=""
if systemctl --user is-active --quiet lexiglanced.service 2>/dev/null; then
	service="systemd"
elif command -v sv >/dev/null 2>&1 && sv status lexiglanced >/dev/null 2>&1; then
	service="runit"
fi

case "$service" in
	systemd) echo "stopping lexiglanced.service..."; systemctl --user stop lexiglanced.service ;;
	runit) echo "stopping lexiglanced (runit)..."; sv stop lexiglanced ;;
	*) if pkill -x lexiglanced 2>/dev/null; then echo "stopping lexiglanced..."; sleep 1; fi ;;
esac
pkill -x lexiglance 2>/dev/null || true

echo "installing into $prefix..."
if [ -w "$prefix" ]; then
	cmake --install "$build" --prefix "$prefix"
else
	sudo cmake --install "$build" --prefix "$prefix"
fi

case "$service" in
	systemd) systemctl --user daemon-reload; systemctl --user start lexiglanced.service; echo "restarted." ;;
	runit) sv start lexiglanced; echo "restarted." ;;
	*) echo "start it again with $prefix/bin/lexiglance" ;;
esac
