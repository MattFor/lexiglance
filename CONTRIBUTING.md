# Contributing

Thanks for helping. Bug reports with screenshots and daemon logs (`lexiglanced --replace --verbose`) are as welcome
as code.

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

```sh
cmake --preset debug          # warnings are errors here
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release` (LTO), `release-native` (tuned for your CPU), `asan` / `clang-asan` (sanitizers), `portable`
(everything but the daemon, for other systems).

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
glossaries, compiled dictionaries and the whole import). With Clang:

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLEXIGLANCE_FUZZ=ON -DLEXIGLANCE_SANITIZE=ON -DLEXIGLANCE_BUILD_GUI=OFF -DLEXIGLANCE_BUILD_DAEMON=OFF
cmake --build build/fuzz
build/fuzz/tests/fuzz/fuzzZip -max_total_time=300 corpus/
```

A crash input found this way belongs in a regression test next to the fix.

## Adding a language

Most languages are a single JSON file in `data/languages`. [docs/languages.md](docs/languages.md) explains the format.

## Style

- Format with the repository's `.clang-format`; CI checks it.
- `clang-tidy` must stay at zero findings (`run-clang-tidy -p build/debug`).
- Comments explain why, not what, and are rare. Prefer clear names over comments.
- New behaviour comes with a test in `tests/` (see `tests/Test.h` for the tiny framework).
- Never add anything that injects input, grabs input beyond what `docs/anticheat.md` describes, or touches other
  processes.

## Pull requests

Describe the user-visible effect.

Changes written with the help of LLM tools are accepted on the same terms as any other: you must understand the
change, have built and tested it, and be able to answer questions about it. Say so in the pull request if a
substantial part was generated.
