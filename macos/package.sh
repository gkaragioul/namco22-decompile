#!/bin/bash
# macos/package.sh -- bundle the built Metal binaries as macOS .app bundles.
#
# Reads the binaries produced by macos/build.sh (in .opencode-work/macos-build)
# and writes user-reviewable .app bundles into macos/output/. ROMs are NOT
# copied: each launcher points at the prepared <game>/extracted directory
# (override with TIMECRIS_ROMS / TIMECRIS2_ROMS) -- the originals stay read-only.
# ROM_ROOT=<dir> points the launchers at <dir>/<app name>/extracted instead
# (macos/install.sh uses ~/Library/Application Support). The game's settings,
# saves and high scores live next to that extracted folder; each start writes
# its log to ~/Library/Logs/<app name>/last-run.log. The game opens as a window
# filling the desktop (menu bar and Dock visible); F11 is fullscreen.
#
# The bundle executable is macos/launcher.c, a compiled Mach-O that execs the
# game: macOS Game Mode never classifies an app whose executable is a script.
#
# SDL goes inside each app (Contents/Frameworks): Homebrew's sdl2-compat and
# the SDL3 it loads (sdl2-compat looks for @loader_path/libSDL3.dylib first),
# with the install names rewritten and the bundle signed again (ad hoc). Homebrew's copies run only on the macOS they were built
# for: a release for older Macs is built with MACOSX_DEPLOYMENT_TARGET=13.0 against an SDL built the same way (PKG_CONFIG_PATH to its
# sdl2.pc for macos/build.sh, SDL3_DYLIB=<its libSDL3.0.dylib> and SDL2_DYLIB=<its libSDL2-2.0.0.dylib> here,
# with MACOSX_DEPLOYMENT_TARGET=13.0 set for the launcher too).
#
# DATA_FROM=<dir> makes the apps self-contained: <dir>/<app name>/extracted
# (the ROMs) and <dir>/<app name>/HD (the HD textures, with macos/hd-fixed
# laid over them) are copied into Contents/Resources, so the app plays from
# wherever it is moved; the player's settings and saves then go to
# ~/Library/Application Support/<app name>. The copies are APFS clones where
# the disk allows (no extra space). DATA_FROM=installed takes them from the
# installed self-contained apps (a rebuild). Such an app holds game data: never publish it.
#
# RELEASE=1 makes the apps for a public download: no game data at all. On the first start the app asks for the player's own MAME
# zip (a macOS file dialog, macos/launcher.c), and the game unpacks it into ~/Library/Application Support/<app name>. No HD pack and
# no upscaler (the HD textures are made from the game's art); Time Crisis II keeps its right-click tutorial panels (macos/hd-fixed).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD_DIR:-.opencode-work/macos-build}"
OUT="${OUTPUT_DIR:-macos/output}"

[ -x "$BUILD/Time Crisis" ] || { echo "build first: ./macos/build.sh"; exit 1; }

package_one() { # <display-name> <bundle-id> <binary> <rom-env> <default-roms> <roms-as-argv1> <icon.icns> <mame-set> <zip help>
    local name="$1" id="$2" bin="$3" env="$4" roms="$5" roms_arg="$6" icon="$7" set="$8" ziphelp="$9"
    if [ -n "${RELEASE:-}" ] && [ -n "${DATA_FROM:-}" ]; then echo "RELEASE and DATA_FROM do not mix: a release carries no game data" >&2; return 1; fi
    [ -n "${ROM_ROOT:-}" ] && roms="$ROM_ROOT/$name/extracted"
    local player=0; if [ -n "${RELEASE:-}" ]; then player=1; roms=extracted; fi   # a release names no folder of this Mac
    local app="$OUT/$name.app"
    if [ -e "$app" ]; then
        echo "refusing to replace existing bundle: $app (set OUTPUT_DIR to a new directory)" >&2
        return 1
    fi
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

    cp "$BUILD/$bin" "$app/Contents/MacOS/$bin"
    cp "$icon" "$app/Contents/Resources/AppIcon.icns"     # macos/icons: made from the games' own frames
    mkdir -p "$app/Contents/Resources/crosshairs"         # Display > Crosshair image (SDL_GetBasePath = Contents/Resources)
    cp macos/crosshairs/*.png "$app/Contents/Resources/crosshairs/"
    cp macos/hd-upscale.sh "$app/Contents/Resources/hd-upscale.sh"   # HD textures: the quit-time upscaler (macos/tex_hd.h)
    mkdir -p "$app/Contents/Resources/sprites"            # replacement sprites (engine/ss22_gl.c: the pedal sign says right click)
    cp macos/sprites/*.png "$app/Contents/Resources/sprites/"
    if [ "$bin" = "Time Crisis II" ]; then                # HD images that must always be there: the tutorial panels saying right click
        mkdir -p "$app/Contents/Resources/hd-fixed"       # (the launcher copies them into the player's HD folder)
        cp macos/hd-fixed/tc2/*.png "$app/Contents/Resources/hd-fixed/"
    fi
    clang -O2 -Wall -o "$app/Contents/MacOS/launcher" macos/launcher.c \
        -DLAUNCH_NAME="\"$name\"" -DLAUNCH_BIN="\"$bin\"" -DLAUNCH_ROM_ENV="\"$env\"" \
        -DLAUNCH_ROMS="\"$roms\"" -DLAUNCH_ROMS_ARG="$roms_arg" \
        -DLAUNCH_PLAYER_ROMS="$player" -DLAUNCH_SET="\"$set\"" -DLAUNCH_ZIP_HELP="\"$ziphelp\""
    if [ "$player" = 1 ]; then rm -f "$app/Contents/Resources/hd-upscale.sh"; fi   # nothing is collected or upscaled on a player's Mac

    bundle_sdl "$app" "$bin"
    if [ -n "${DATA_FROM:-}" ]; then
        local src="$DATA_FROM/$name"
        [ "$DATA_FROM" = installed ] && src="/Applications/$name.app/Contents/Resources"   # rebuild: the data the installed app carries
        [ -d "$src/extracted" ] || { echo "no ROMs at $src/extracted" >&2; return 1; }
        cp -cR "$src/extracted" "$app/Contents/Resources/extracted" 2>/dev/null || cp -R "$src/extracted" "$app/Contents/Resources/extracted"
        if [ -d "$src/HD" ]; then
            cp -cR "$src/HD" "$app/Contents/Resources/HD" 2>/dev/null || cp -R "$src/HD" "$app/Contents/Resources/HD"
            [ -d "$app/Contents/Resources/hd-fixed" ] && cp -f "$app/Contents/Resources/hd-fixed/"*.png "$app/Contents/Resources/HD/"
        fi
        rm -f "$app/Contents/Resources/hd-upscale.sh"    # a finished pack: nothing is collected or upscaled inside the app
        echo "  self-contained: $(ls "$app/Contents/Resources/extracted" | wc -l | tr -d ' ') ROM files, $(ls "$app/Contents/Resources/HD" 2>/dev/null | wc -l | tr -d ' ') HD textures"
    fi

    cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$id</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleVersion</key><string>${APP_VERSION:-1.1}</string>
  <key>CFBundleShortVersionString</key><string>${APP_VERSION:-1.1}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>LSSupportsGameMode</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
EOF
    xattr -cr "$app"
    codesign --force --sign - "$app" >/dev/null 2>&1   # seals the bundle (the Frameworks and the game were signed one by one)
    codesign --verify --strict "$app" || { echo "signature check failed: $app" >&2; return 1; }
    echo "packaged $app"
}

bundle_sdl() { # <app> <binary>: Homebrew's SDL into Contents/Frameworks, the game pointed at it
    local app="$1" bin="$2" fw="$1/Contents/Frameworks"
    local sdl2 sdl3
    sdl2=$(otool -L "$app/Contents/MacOS/$bin" | awk '/libSDL2-2.0.0.dylib/ {print $1; exit}')
    [ -n "$sdl2" ] || { echo "the game does not link SDL2?" >&2; return 1; }
    sdl3="${SDL3_DYLIB:-$(brew --prefix sdl3 2>/dev/null || echo /opt/homebrew/opt/sdl3)/lib/libSDL3.0.dylib}"   # SDL3_DYLIB: one built for older macOS
    [ -f "$sdl3" ] || { echo "no SDL3 at $sdl3 (brew install sdl3)" >&2; return 1; }
    mkdir -p "$fw"
    cp "${SDL2_DYLIB:-$(realpath "$sdl2")}" "$fw/libSDL2-2.0.0.dylib"   # SDL2_DYLIB: the sdl2-compat the game was linked with, when it links @rpath
    cp "$(realpath "$sdl3")" "$fw/libSDL3.dylib"
    chmod u+w "$fw"/*.dylib
    install_name_tool -id @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$fw/libSDL2-2.0.0.dylib" 2>/dev/null
    install_name_tool -id @loader_path/libSDL3.dylib "$fw/libSDL3.dylib" 2>/dev/null
    # the game is re-pointed and signed outside the bundle: signed in place it would be sealed to the bundle as it is now
    local tmp; tmp=$(mktemp -d)
    cp "$app/Contents/MacOS/$bin" "$tmp/game"
    install_name_tool -change "$sdl2" @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$tmp/game" 2>/dev/null
    if otool -L "$tmp/game" | grep -q -E '/opt/homebrew|/Volumes/|/Users/'; then echo "the game still links a library outside the app" >&2; return 1; fi
    xattr -c "$tmp/game"; codesign --force --sign - --identifier "$(basename "$app" .app | tr ' ' '-')-game" "$tmp/game" >/dev/null 2>&1
    mv -f "$tmp/game" "$app/Contents/MacOS/$bin"; rmdir "$tmp"
    xattr -cr "$app"                                    # Finder info and the like: codesign refuses files carrying them
    codesign --force --sign - "$fw/libSDL3.dylib" "$fw/libSDL2-2.0.0.dylib" >/dev/null 2>&1
}

package_one "Time Crisis" "com.namco22.timecris" "Time Crisis" \
    "TIMECRIS_ROMS" "$(cd timecris && pwd)/extracted" 1 macos/icons/tc1.icns timecris "timecris.zip"
if [ -x "$BUILD/Time Crisis II" ]; then
    package_one "Time Crisis II" "com.namco22.timecris2" "Time Crisis II" \
        "TIMECRIS2_ROMS" "$(cd tc2 && pwd)/extracted" 0 macos/icons/tc2.icns timecrs2 "timecrs2.zip (with namco_tssio.zip, if your set has it)"
fi
echo "done -> $OUT"
