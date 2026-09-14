# Security policy

## Supported versions

Security fixes go into the latest release.

## Reporting a vulnerability

Please report privately through
[GitHub security advisories](https://github.com/MattFor/lexiglance/security/advisories/new) rather than a public
issue. Include the version, your system and, if you can, a way to reproduce it.

## Scope

Areas where a flaw matters most:

- Dictionary import: `.zip` archives and JSON from untrusted sources are parsed by the importer.
- The daemon's local socket (`$XDG_RUNTIME_DIR/lexiglance/daemon.sock`, user-only permissions) and its JSON protocol.
- Network access: audio sources, AnkiConnect and model downloads.
- Screen reading: OCR only reads around the pointer while the trigger is held (see `docs/anticheat.md`).
