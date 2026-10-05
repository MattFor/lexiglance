# Starting the daemon automatically

`lexiglanced` belongs to your graphical session: it needs `DISPLAY` (X11) and must run as you, not as root. Pick one
of the ways below; the first works everywhere.

Installed files live in `<prefix>/share/lexiglance/services/` (the systemd unit also in `<prefix>/lib/systemd/user/`),
with the daemon's installed path already filled in.

## Desktop autostart (any desktop, recommended)

**Overview -> Start Lexiglance automatically when I log in** in the settings application writes
`~/.config/autostart/lexiglance-daemon.desktop` (on Windows the `Run` registry key, on macOS a LaunchAgent). Every
XDG desktop starts it with the session. The same file ships as `services/xdg/lexiglance-daemon.desktop`.

**Also start this window, hidden in the tray**, below it, adds `~/.config/autostart/lexiglance-tray.desktop` (on
Windows the `Lexiglance tray` value of the same key), which starts the settings application with `--tray`.

Both, and the applications menu entry, start the copy of Lexiglance that was installed last. A release (the AppImage,
a package, the Windows setup or portable folder, an installed prefix) points them at itself when it starts, and stops
another copy's settings window and daemon, so the one just installed is the one that runs. A copy run from its build
tree leaves them alone, unless they start a program that is gone; `.github/scripts/dev-install.sh` (or `.ps1`) puts
them back on the build it installs with `lexiglance --claim-entries`. Whether each one is on stays as you chose.

## Run as administrator (Windows)

While a program that runs as administrator is in front (many games and their launchers do), Windows keeps its keys
and clicks from programs that do not, so the trigger does nothing over it; Health names such programs. **Overview ->
Run as administrator** makes the daemon run as administrator too. Windows asks once, when it is turned on: that sets
up a scheduled task, `Lexiglance as administrator (<user>)`, which starts `lexiglanced.exe --from-task` with the
highest privileges. From then on a daemon started without them (by the autostart above, the settings application or
`lexiglanced --replace`) starts that task and makes way for the daemon it starts, with no prompt. The settings
application itself runs as before.

The task starts on demand only, as you, in your session. You may start and delete it, but changing what it starts
takes an administrator. Turning the option off, uninstalling, or `lexiglanced --run-as-administrator off` deletes it;
`lexiglanced --run-as-administrator status` shows what it starts.

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
