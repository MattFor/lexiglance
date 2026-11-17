# Contributing

Thanks for helping. Bug reports with screenshots and daemon logs (`lexiglanced --replace --verbose`) are as welcome
as code: **Report a problem** (Overview -> Health, About or the tray menu) or `lexiglancectl report` gathers what is
needed into one zip.

## Debugging

Developer mode (About: click the version seven times, or `lexiglance --developer`) adds a Developer page: the pipeline
trace with each stage's timings, a capture inspector that shows what OCR read, scans, lookups and problems simulated
through the daemon's own pipeline, the log as it is written and an IPC console. `lexiglancectl trace` and
`lexiglancectl debug ...` do the same from a terminal. [docs/debugging.md](docs/debugging.md) describes it all.

## Building

Dependencies:

| System                   | Packages                                                                                                                                                                                           |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Void                     | `base-devel cmake ninja zlib-devel cairo-devel pango-devel libX11-devel libXi-devel libXfixes-devel libXrandr-devel libXext-devel at-spi2-core-devel libcurl-devel qt6-base-devel`                 |
| Arch                     | `base-devel cmake ninja zlib cairo pango libx11 libxi libxfixes libxrandr libxext at-spi2-core curl qt6-base`                                                                                      |
| Debian 13 / Ubuntu 24.04 | `build-essential g++-14 cmake ninja-build zlib1g-dev libcairo2-dev libpango1.0-dev libx11-dev libxi-dev libxfixes-dev libxrandr-dev libxext-dev libatspi2.0-dev libcurl4-openssl-dev qt6-base-dev` |
| Fedora 41+               | `gcc-c++ cmake ninja-build zlib-devel cairo-devel pango-devel libX11-devel libXi-devel libXfixes-devel libXrandr-devel libXext-devel at-spi2-core-devel libcurl-devel qt6-qtbase-devel`            |

GCC 14 or Clang 18 (or newer) is needed for C++26, CMake 3.30 or newer, and Qt 6.4 or newer for the settings
application. Ubuntu 24.04's own CMake is too old: `pipx install cmake` gives a current one.

On macOS: Xcode's command line tools (`xcode-select --install`), `pipx install cmake ninja meson`, Pango and Cairo
built by `bash .github/scripts/macos-deps.sh ~/lexiglance-deps` (for macOS 13.3, as the release has them) and Qt 6 from
Qt's installer, both passed to the configure step as `-DCMAKE_PREFIX_PATH="<Qt>/6.8.3/macos;$HOME/lexiglance-deps"`.
[docs/porting.md](docs/porting.md#macos) has the details.

```sh
cmake --preset debug          # warnings are errors here
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release` (LTO), `release-native` (tuned for your CPU), `asan` / `clang-asan` (sanitizers), `portable`
(everything but the daemon, for other systems).

## Trying a change on your own desktop

Builds and then replaces the Lexiglance this machine actually runs, the copy its login autostart (or service) starts,
restarting whatever was running:

```sh
bash .github/scripts/dev-install.sh              # what the autostart runs: a build tree is rebuilt in place, an
                                                 # installed copy is installed over (sudo only if needed)
bash .github/scripts/dev-install.sh debug ~/.local
```

```powershell
pwsh .github/scripts/dev-install.ps1             # release preset, into what the autostart runs (else where setup.exe put it)
pwsh .github/scripts/dev-install.ps1 -Preset debug -SkipBuild
```

The autostart entries, the applications menu entry (Start menu shortcut) and the links in `~/.local/bin` start the
copy it installed from then on, also when a release took them over in between (**Install release**, or an AppImage
started since): with the autostart on such a release, the build tree the script put in place last is rebuilt and put
back. It also points out other copies earlier on `PATH`, which the terminal or the applications menu would run instead.

On Windows the daemon and the settings application are stopped first, because Windows cannot overwrite a running
program; on Linux the daemon's supervisor is asked to stop and start it. Settings and dictionaries live outside the
install directory and are left alone either way.

## Releasing

The version in `CMakeLists.txt` (`project(... VERSION x.y.z)`) is the one the in-app updater, About and the release
workflow go by, so it goes up with every release, and the release tag is exactly `v<that version>`. A pre-push hook
warns when a push to the default branch keeps the version the remote already has, or a tag does not match; turn it on
once per clone:

```sh
git config core.hooksPath .githooks
```

At a terminal it asks whether to push anyway; elsewhere (an IDE) the push stops, and `git push --no-verify` pushes
anyway.

## Building the Windows installer

On Windows, from an install prefix rather than over an existing install:

```powershell
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix build/stage
pwsh .github/scripts/windows-installer.ps1 build/release build/stage build/dist
```

## Building the macOS disk image

From a release build against the `macos-deps.sh` libraries, as the release makes `Lexiglance-macos.dmg` (with one
build directory per architecture, `dmg` merges them into a universal app; `test` fails on a library built for a newer
macOS than the app's 13.3, such as Homebrew's or MacPorts'):

```sh
bash .github/scripts/macos-package.sh bundle build/release build/arm64/Lexiglance.app
bash .github/scripts/macos-package.sh dmg build/dist build/arm64/Lexiglance.app
bash .github/scripts/macos-package.sh test build/dist/Lexiglance-macos.dmg
```

## Cross-compiling for Windows

With MinGW-w64 (GCC 14) and a static zlib built for it (Fedora: `mingw64-gcc-c++ mingw64-zlib-static`):

```sh
cmake -S . -B build/windows -G Ninja --toolchain cmake/toolchains/mingw-w64.cmake \
      -DLEXIGLANCE_BUILD_DAEMON=OFF -DLEXIGLANCE_BUILD_GUI=OFF
cmake --build build/windows
ctest --test-dir build/windows   # runs the Windows test binary under wine
```

## Fuzzing

Parsers of untrusted input have libFuzzer targets (JSON, ZIP, configuration, IPC framing, dictionary style sheets,
glossaries, compiled dictionaries, the whole import, language files, lookups of any text with the answer the daemon
writes, and the translation tokenizer). With Clang:

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLEXIGLANCE_FUZZ=ON -DLEXIGLANCE_SANITIZE=ON -DLEXIGLANCE_BUILD_GUI=OFF -DLEXIGLANCE_BUILD_DAEMON=OFF
cmake --build build/fuzz
build/fuzz/tests/fuzz/fuzzZip -max_total_time=300 corpus/
```

A crash input found this way belongs in a regression test next to the fix.

## Measuring OCR

A change to how text is read off the screen (`src/ocr`, `src/platform/ocr`) is measured rather than judged by a few
screenshots: what helps one font often costs another. The benchmark draws lines of Japanese, Russian, Korean and
Greek in some fifty fonts, sizes and styles, has the build's own screen reading read them, and counts the characters
that came out wrong. Run it on the code before the change and after it:

```sh
cmake --preset release -DLEXIGLANCE_BUILD_TOOLS=ON && cmake --build --preset release
QUICK=1 bash tools/ocr-bench/run.sh build/release before          # a fifth of the pictures; without QUICK=1, all
QUICK=1 bash tools/ocr-bench/run.sh build/release after before    # with the change: beside the results before it
```

It needs PaddleOCR downloaded (**Scanning -> Download PaddleOCR**), Python with Pillow and NumPy, and fontconfig;
the fonts and the recognisers of the other languages are downloaded on the first run. `tools/ocr-bench/run.sh` says
more, `--beside` among it: pointing next to the text, where nothing may be read.

## Trying the keys and the wheel

The trigger, the sentence key and the wheel are tried on a real daemon in a display of its own (Xvfb), so nothing on
your desktop is touched: xdotool presses and turns as fast as an impatient hand, and the daemon's log says whether
every press counted.

```sh
bash tools/input-test.sh build/release/apps/daemon/lexiglanced
```

It needs Xvfb, xdotool and ImageMagick, and PaddleOCR, the Japanese translation model and a Japanese dictionary
installed.

## Adding a language

Most languages are a single JSON file in `data/languages`. [docs/languages.md](docs/languages.md) explains the format.

## Style

- Format with the repository's `.clang-format`; CI checks it.
- `clang-tidy` must stay at zero findings (`clang-tidy -p build --quiet --warnings-as-errors='*'` on the same sources CI
  checks). Only tidy platform sources for the OS you built (`src/platform/linux` on Linux, `src/platform/windows` on
  Windows): the wrong OS fails the first missing system header and then floods false positives.
- Comments explain why, not what, and are rare. Prefer clear names over comments.
- New behaviour comes with a test in `tests/` (see `tests/Test.h` for the tiny framework).
- Never add anything that injects input, grabs input beyond what `docs/anticheat.md` describes, or touches other
  processes.

## Pull requests

Describe the user-visible effect.

Changes written with the help of LLM tools are accepted on the same terms as any other: you must understand the
change, have built and tested it, and be able to answer questions about it. Say so in the pull request if a
substantial part was generated.
