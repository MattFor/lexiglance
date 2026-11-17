#!/usr/bin/env bash
# Builds the libraries the macOS programs link (Pango, Cairo, HarfBuzz, GLib and theirs) from pinned release sources
# into a prefix, for this Mac's architecture and for macOS 13.3, drawing with Core Text and Quartz (no FreeType,
# Fontconfig or X11). MacPorts' and Homebrew's builds run only on the macOS they were built on (15), and so would the
# app. CI caches the prefix (keyed on this file).
#   [JOBS=n] bash .github/scripts/macos-deps.sh <prefix>
# Needs meson and ninja on PATH. Lexiglance then builds with <prefix> in CMAKE_PREFIX_PATH: <prefix>/bin/pkg-config
# looks only in <prefix>, so nothing of Homebrew's or MacPorts' gets in.
set -euo pipefail

# The oldest macOS the app runs on, as in CMakeLists.txt (macos-package.sh test checks every library against it).
export MACOSX_DEPLOYMENT_TARGET=13.3
# Meson downloads GLib's subprojects, and python.org's Python has no certificates of its own.
export SSL_CERT_FILE=${SSL_CERT_FILE:-/etc/ssl/cert.pem}
[ $# = 1 ] || {
    echo "usage: $0 <prefix>" >&2
    exit 1
}
prefix=$(mkdir -p "$1" && cd "$1" && pwd)
jobs=${JOBS:-$(sysctl -n hw.ncpu)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Downloads and unpacks a release: file name, sha256, then URLs to try in turn (the last is MacPorts' copy). A download
# slower than 50 kB/s gives up, so the next one is tried (cairographics.org has crawled at 35 kB/s and hung).
fetch() {
    local file=$1 sum=$2 url
    shift 2
    for url in "$@"; do
        curl -fsSL --retry 2 --connect-timeout 30 --speed-limit 50000 --speed-time 30 -o "$work/$file" "$url" && break
    done
    echo "$sum  $work/$file" | shasum -a 256 -c
    tar -xf "$work/$file" -C "$work"
}

# Builds a meson project. Below, -Dauto_features=disabled keeps optional libraries found on the build Mac (Homebrew's
# FreeType, X11) out, and --wrap-mode=nofallback keeps a missing one from being fetched from a development branch.
build() {
    local dir=$work/$1
    shift
    meson setup "$dir/_build" "$dir" --prefix="$prefix" --libdir=lib --buildtype=release -Dstrip=true "$@"
    meson compile -C "$dir/_build" -j "$jobs"
    meson install -C "$dir/_build"
}

fetch pkgconf-3.0.10.tar.gz d01a1b82f0884112d08c6dfad46f43f415d070a6a5a6aaa77ebf2674e973a9b5 \
    https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.10.tar.gz \
    https://distfiles.macports.org/pkgconf/pkgconf-3.0.10.tar.gz
build pkgconf-3.0.10
ln -sf pkgconf "$prefix/bin/pkg-config"
export PATH="$prefix/bin:$PATH"
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR

# macOS's own libffi and zlib (Homebrew's builds use them too): the SDK has them, without pkg-config files.
mkdir -p "$prefix/lib/pkgconfig"
printf 'Name: libffi\nDescription: macOS libffi\nVersion: 3.4\nLibs: -lffi\nCflags: -I%s/usr/include/ffi\n' \
    "$(xcrun --show-sdk-path)" >"$prefix/lib/pkgconfig/libffi.pc"
printf 'Name: zlib\nDescription: macOS zlib\nVersion: 1.2.12\nLibs: -lz\n' >"$prefix/lib/pkgconfig/zlib.pc"

# PCRE2 and a no-op libintl (GLib's messages stay English) come from GLib's own pinned subprojects.
fetch glib-2.88.3.tar.xz ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b \
    https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz \
    https://distfiles.macports.org/glib2/glib-2.88.3.tar.xz
build glib-2.88.3 -Dauto_features=disabled -Dtests=false -Dglib_debug=disabled -Dforce_fallback_for=libpcre2-8,intl

fetch fribidi-1.0.18.tar.xz 1de5e8073d035388b71d0b2d452b1da1f8caeea8a7702997b401fe1b6250727f \
    https://github.com/fribidi/fribidi/releases/download/v1.0.18/fribidi-1.0.18.tar.xz \
    https://distfiles.macports.org/fribidi/fribidi-1.0.18.tar.xz
build fribidi-1.0.18 --wrap-mode=nofallback -Ddocs=false -Dbin=false -Dtests=false

fetch harfbuzz-15.1.0.tar.xz 0fc7c565443a5f4be7da65ac5ee5bd737d9b158dc908708b7689a0a5796b0c8d \
    https://github.com/harfbuzz/harfbuzz/releases/download/15.1.0/harfbuzz-15.1.0.tar.xz \
    https://distfiles.macports.org/harfbuzz/harfbuzz-15.1.0.tar.xz
build harfbuzz-15.1.0 --wrap-mode=nofallback -Dauto_features=disabled -Dglib=enabled -Dcoretext=enabled \
    -Dtests=disabled -Dutilities=disabled -Dsubset=disabled -Draster=disabled -Dvector=disabled -Dgpu=disabled

# Pixman's SIMD code stays on auto, except MMX (SSE2 does the same), which MacPorts turns off too.
fetch pixman-0.46.4.tar.xz a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239 \
    https://www.x.org/archive/individual/lib/pixman-0.46.4.tar.xz \
    https://distfiles.macports.org/pixman/pixman-0.46.4.tar.xz
build pixman-0.46.4 --wrap-mode=nofallback -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled \
    -Dopenmp=disabled -Dmmx=disabled

fetch libpng-1.6.59.tar.xz d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389 \
    https://download.sourceforge.net/libpng/libpng-1.6.59.tar.xz \
    https://distfiles.macports.org/libpng/libpng-1.6.59.tar.xz
(cd "$work/libpng-1.6.59" && ./configure --prefix="$prefix" --disable-static --disable-tools &&
    make -j "$jobs" install-strip)

fetch cairo-1.18.6.tar.xz 1c767308174337a74694da0f3ec069c271452163a1ef4540964c50c301f157d4 \
    https://www.cairographics.org/releases/cairo-1.18.6.tar.xz \
    https://distfiles.macports.org/cairo/cairo-1.18.6.tar.xz
build cairo-1.18.6 --wrap-mode=nofallback -Dauto_features=disabled -Dquartz=enabled -Dpng=enabled

fetch pango-1.58.2.tar.xz 342385b6ca3b7c73455d7c80a13b7dbe4489e00bc3bd4c5bd6ed4dce421e374a \
    https://download.gnome.org/sources/pango/1.58/pango-1.58.2.tar.xz \
    https://distfiles.macports.org/pango/pango-1.58.2.tar.xz
build pango-1.58.2 --wrap-mode=nofallback -Dauto_features=disabled -Dcairo=enabled -Dbuild-testsuite=false \
    -Dbuild-examples=false
