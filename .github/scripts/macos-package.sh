#!/usr/bin/env bash
# Builds Lexiglance-macos.dmg: Lexiglance.app with Qt and every library it loads inside, ad-hoc signed, beside a link
# to /Applications to drag it onto. Used by CI and the release; the in-app updater downloads the disk image by this
# name and replaces the app from it.
#   bash .github/scripts/macos-package.sh bundle <build directory> <Lexiglance.app to make>
#   bash .github/scripts/macos-package.sh dmg <output directory> <Lexiglance.app>...
#   bash .github/scripts/macos-package.sh test <Lexiglance-macos.dmg>
# bundle installs one build (one architecture) into an app and copies Qt and the libraries into it (macdeployqt).
# dmg merges the apps of several architectures into one universal app (lipo), signs it and makes the disk image.
# test checks a disk image as a user gets it: the app copied out of it starts, and loads nothing from outside itself
# and the system, in every architecture this Mac runs.
set -euo pipefail

# The daemon is a helper app of its own (no Dock icon); in Contents/MacOS LaunchServices would take it for the app.
daemon=Contents/Helpers/lexiglanced.app/Contents/MacOS/lexiglanced

die() {
    echo "error: $*" >&2
    exit 1
}

# Mach-O files under a folder (programs, libraries, plugins, framework binaries).
macho_files() {
    find "$1" -type f -print0 | while IFS= read -r -d '' file; do
        lipo -archs "$file" >/dev/null 2>&1 && echo "$file"
    done
}

# The folders a program or library searches for @rpath/ libraries (LC_RPATH) that are not relative to it.
foreign_rpaths() {
    otool -l "$1" | awk '$1 == "cmd" { rpath = ($2 == "LC_RPATH") } rpath && $1 == "path" { print $2 }' | grep -v '^@' || true
}

# Fails when a program or library refers to a library outside the app and the system, or searches a folder outside the
# app: on another Mac that would be missing, or someone else's (/opt/homebrew, /opt/local, /usr/local, the build's Qt).
check_closure() {
    local file bad=0
    while IFS= read -r file; do
        # Only the indented lines are references: on Apple silicon a universal file prints a header per architecture.
        while IFS= read -r line; do
            echo "$file: $line" >&2
            bad=1
        done < <(otool -L "$file" | awk '/^\t/ {print $1}' | grep -Ev '^(/System/Library/|/usr/lib/|@executable_path/|@loader_path/|@rpath/)' || true)
        while IFS= read -r line; do
            echo "$file: LC_RPATH $line" >&2
            bad=1
        done < <(foreign_rpaths "$file")
    done < <(macho_files "$1")
    [ "$bad" = 0 ]
}

# Fails when a program or library under a folder needs a newer macOS than $2, in any of its architectures: dyld refuses
# it on an older one, so the app would not start there (LC_BUILD_VERSION's minos; LC_VERSION_MIN_MACOSX before 10.14).
check_minos() {
    local file bad=0
    while IFS= read -r file; do
        while IFS= read -r line; do
            echo "$file: needs macOS $line" >&2
            bad=1
        done < <(otool -arch all -l "$file" | awk -v want="$2" '
            function newer(a, b, x, y, i) {
                split(a, x, "."); split(b, y, ".")
                for (i = 1; i <= 3; i++) if (x[i] + 0 != y[i] + 0) return x[i] + 0 > y[i] + 0
                return 0
            }
            $1 == "cmd" { cmd = $2 }
            cmd == "LC_BUILD_VERSION" && $1 == "minos" && newer($2, want) { print $2 }
            cmd == "LC_VERSION_MIN_MACOSX" && $1 == "version" && newer($2, want) { print $2 }')
    done < <(macho_files "$1")
    [ "$bad" = 0 ]
}

bundle() {
    local build=$1 app=$2 stage qt program file rpath
    [ -f "$build/CMakeCache.txt" ] || die "$build is not a build directory"
    qt=$(sed -n 's|^Qt6_DIR:PATH=||p' "$build/CMakeCache.txt")
    [ -n "$qt" ] || die "$build was configured without the settings application (Qt)"
    stage=$(mktemp -d)
    cmake --install "$build" --prefix "$stage" --strip
    for program in Contents/MacOS/lexiglance Contents/MacOS/lexiglancectl "$daemon"; do
        [ -x "$stage/Lexiglance.app/$program" ] || die "${program##*/} was not built"
    done
    # Qt's frameworks and plugins, and every other library the three programs load, with their names rewritten to the
    # app's Frameworks folder.
    "$qt/../../../bin/macdeployqt" "$stage/Lexiglance.app" -always-overwrite \
        -executable="$stage/Lexiglance.app/$daemon" \
        -executable="$stage/Lexiglance.app/Contents/MacOS/lexiglancectl"
    # Search paths into where the libraries came from (MacPorts' brotli has /opt/local/lib): what they were for is
    # rewritten, and on a user's Mac they could find another copy.
    while IFS= read -r file; do
        while IFS= read -r rpath; do
            install_name_tool -delete_rpath "$rpath" "$file"
        done < <(foreign_rpaths "$file")
    done < <(macho_files "$stage/Lexiglance.app")
    check_closure "$stage/Lexiglance.app" || die "the app refers to files outside itself and the system (above)"
    rm -rf "$app"
    mkdir -p "$(dirname "$app")"
    ditto "$stage/Lexiglance.app" "$app"
    rm -rf "$stage"
}

# Adds the architectures of the app $2 to the app $1: a program or library they both have becomes one file with both.
merge() {
    local into=$1 from=$2 file target
    (cd "$from" && find . -type f -print0) | while IFS= read -r -d '' file; do
        target="$into/$file"
        if [ ! -e "$target" ]; then
            mkdir -p "$(dirname "$target")"
            ditto "$from/$file" "$target"
        elif lipo -archs "$target" >/dev/null 2>&1 && [ "$(lipo -archs "$target")" != "$(lipo -archs "$from/$file")" ]; then
            lipo -create "$target" "$from/$file" -output "$target.universal"
            mv "$target.universal" "$target"
        fi
    done
}

dmg() {
    local output=$1 work app other
    shift
    [ $# -ge 1 ] || die "no app given"
    work=$(mktemp -d)
    app="$work/root/Lexiglance.app"
    mkdir -p "$work/root"
    ditto "$1" "$app"
    shift
    for other in "$@"; do
        merge "$app" "$other"
    done
    echo "architectures: $(lipo -archs "$app/Contents/MacOS/lexiglance")"
    check_closure "$app" || die "the app refers to files outside itself and the system (above)"
    # No Developer ID: an ad-hoc signature, without which Apple silicon runs none of it.
    codesign --force --deep --sign - "$app"
    codesign --verify --deep --strict "$app"
    ln -s /Applications "$work/root/Applications"
    mkdir -p "$output"
    # LZMA (macOS 10.15+): a third smaller than zlib. hdiutil fails now and then on CI runners ("Resource busy").
    for attempt in 1 2 3; do
        hdiutil create -volname Lexiglance -srcfolder "$work/root" -fs HFS+ -format ULMO -ov "$output/Lexiglance-macos.dmg" && break
        [ "$attempt" = 3 ] && die "hdiutil could not make the disk image"
        sleep 10
    done
    rm -rf "$work"
    echo "$output/Lexiglance-macos.dmg"
}

test_dmg() {
    local image=$1 work mount app arch failures=0
    # The real path (/private/var/...), as dyld reports it.
    work=$(cd "$(mktemp -d)" && pwd -P)
    mount="$work/mount"
    mkdir "$mount"
    hdiutil attach -nobrowse -readonly -mountpoint "$mount" "$image" >/dev/null
    # shellcheck disable=SC2064 # the folders of this run, fixed now
    trap "hdiutil detach '$mount' -force >/dev/null 2>&1 || true; rm -rf '$work'" EXIT
    check() {
        if "${@:2}"; then echo "ok    $1"; else
            echo "FAIL  $1"
            failures=$((failures + 1))
        fi
    }
    # Every library the program loaded (DYLD_PRINT_LIBRARIES: "dyld[pid]: <uuid> path") came from the app or the system.
    own_libraries() {
        local loaded
        loaded=$(sed -nE 's/^dyld\[[0-9]+\]: <[^>]*> //p' "$1")
        [ -n "$loaded" ] && ! grep -Ev "^($app/|/System/Library/|/usr/lib/|/usr/bin/arch$)" <<<"$loaded"
    }
    check 'the disk image links to /Applications' test "$(readlink "$mount/Applications")" = /Applications
    app="$work/Lexiglance.app"
    ditto "$mount/Lexiglance.app" "$app"
    check 'the signature is valid' codesign --verify --deep --strict "$app"
    check 'nothing refers to files outside the app and the system' check_closure "$app"
    local minimum
    minimum=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist")
    check "every program and library runs on macOS $minimum, the app's minimum" check_minos "$app" "$minimum"
    # A portable home, so the test leaves the user's settings and dictionaries alone.
    export LEXIGLANCE_HOME="$work/home"
    local fixture conjugated=食べさせられなかった lemma=食べる
    fixture="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tests/data/fixture"
    for arch in $(lipo -archs "$app/Contents/MacOS/lexiglance"); do
        if ! arch "-$arch" /usr/bin/true 2>/dev/null; then
            echo "skip  $arch: this Mac does not run it"
            continue
        fi
        run() { DYLD_PRINT_LIBRARIES=1 arch "-$arch" "$@"; }
        check "$arch: the daemon starts" run "$app/$daemon" --version 2>"$work/daemon.log"
        check "$arch: the daemon loads only its own libraries" own_libraries "$work/daemon.log"
        check "$arch: lexiglancectl imports a dictionary" run "$app/Contents/MacOS/lexiglancectl" import "$fixture" 2>"$work/ctl.log"
        check "$arch: lexiglancectl loads only its own libraries" own_libraries "$work/ctl.log"
        check "$arch: a conjugated word is found" grep -q "$lemma" <(arch "-$arch" "$app/Contents/MacOS/lexiglancectl" lookup "$conjugated")
        rm -f "$work/about.png"
        # The daemon it starts reads nothing with OCR: an engine installed apart (Homebrew's or MacPorts' Tesseract) is
        # loaded from there on purpose.
        mkdir -p "$LEXIGLANCE_HOME/config" && echo '{"scan":{"ocr":"off"}}' >"$LEXIGLANCE_HOME/config/config.json"
        check "$arch: the settings application draws a page" run "$app/Contents/MacOS/lexiglance" --screenshot about "$work/about.png" 100 2>"$work/gui.log"
        check "$arch: the page is an image" test "$(stat -f %z "$work/about.png" 2>/dev/null || echo 0)" -gt 10000
        check "$arch: the settings application loads only its own libraries" own_libraries "$work/gui.log"
        # The settings application started the daemon, which would keep running (and this step's output open).
        pkill -f "$app/$daemon" || true
        rm -rf "$LEXIGLANCE_HOME"
    done
    [ "$failures" = 0 ] || die "$failures checks failed"
    echo 'the disk image works on its own'
}

case "${1:-}:$#" in
    bundle:3) bundle "$2" "$3" ;;
    dmg:[3-9]) dmg "${@:2}" ;;
    test:2) test_dmg "$2" ;;
    *) die "usage: $0 bundle <build directory> <Lexiglance.app> | dmg <output directory> <Lexiglance.app>... | test <dmg>" ;;
esac
