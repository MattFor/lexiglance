# Starting the daemon automatically

`lexiglanced` belongs to your graphical session: it needs `DISPLAY` (X11) and must run as you, not as root. Pick one
of the ways below; the first works everywhere.

Installed files live in `<prefix>/share/lexiglance/services/` (the systemd unit also in `<prefix>/lib/systemd/user/`),
with the daemon's installed path already filled in.

## Desktop autostart (any desktop, recommended)

**Overview -> Start Lexiglance automatically when I log in** in the settings application writes
`~/.config/autostart/lexiglance-daemon.desktop` (on Windows the `Run` registry key, on macOS a LaunchAgent). Every
XDG desktop starts it with the session. The same file ships as `services/xdg/lexiglance-daemon.desktop`.

## systemd (user service)

```sh
systemctl --user enable --now lexiglanced.service
```

The unit is bound to `graphical-session.target`, so it starts and stops with the desktop session. If your session
does not import `DISPLAY` into the user manager, add `systemctl --user import-environment DISPLAY XAUTHORITY` to your
session startup.

## runit (Void Linux and others)

runit services run as root by default; run a per-user supervision tree from your session instead:

```sh
mkdir -p ~/.local/service
cp -r <prefix>/share/lexiglance/services/runit/lexiglanced ~/.local/service/
# in your session autostart (e.g. ~/.xinitrc, or the desktop's autostart settings):
runsvdir -P ~/.local/service &
```

With [turnstile](https://github.com/chimera-linux/turnstile), copy the directory to `~/.config/service/` instead.
`sv status ~/.local/service/lexiglanced` shows its state.

## OpenRC (user services, OpenRC 0.60+)

```sh
mkdir -p ~/.config/rc/init.d
cp <prefix>/share/lexiglance/services/openrc/lexiglanced ~/.config/rc/init.d/
rc-update --user add lexiglanced default
```

If the user service manager does not know the display, add `export DISPLAY=:0` to `~/.config/rc/conf.d/lexiglanced`.

## dinit (user services)

```sh
mkdir -p ~/.config/dinit.d
cp <prefix>/share/lexiglance/services/dinit/lexiglanced ~/.config/dinit.d/
dinitctl enable lexiglanced
```

## s6 / s6-rc

`services/s6/lexiglanced/` is an s6-rc long-run service definition (`type`, `run`). Add it to a user s6-rc source
directory, compile the database and start it with `s6-rc -u change lexiglanced`.

## Checking

`lexiglancectl status` answers once the daemon runs. Its log goes to standard error, which the service manager keeps
(`journalctl --user -u lexiglanced`, `svlogd`, ...); add `--verbose` to the command for debug output.
