#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dmg_script="$repo_root/scripts/macos_create_dmg.sh"
desktop_cmake="$repo_root/cmake/acecode_desktop.cmake"
dmg_background="$repo_root/assets/macos/acecode-dmg-background.svg"

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-dmg-layout-test.XXXXXX")"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

expect_status() {
    local expected="$1"
    local label="$2"
    shift 2

    local output="$temporary_root/output.txt"
    local actual=0
    "$@" >"$output" 2>&1 || actual=$?
    if [[ "$actual" -ne "$expected" ]]; then
        echo "$label: expected exit $expected, got $actual" >&2
        sed -n '1,120p' "$output" >&2
        exit 1
    fi
}

bash -n "$dmg_script"
expect_status 0 "DMG help" bash "$dmg_script" --help
expect_status 2 "unknown DMG argument" bash "$dmg_script" --unknown
expect_status 2 "obsolete installer argument" \
    bash "$dmg_script" --installer "$temporary_root/Install ACECode.app"
expect_status 2 "obsolete instructions argument" \
    bash "$dmg_script" --instructions "$temporary_root/README.txt"

test -f "$dmg_background"
grep -Fq 'MACOSX_PACKAGE_LOCATION "Resources"' "$desktop_cmake"

grep -Fq -- '-format UDRW' "$dmg_script"
grep -Fq '/usr/bin/osascript' "$dmg_script"
grep -Fq 'set background picture of iconOptions' "$dmg_script"
grep -Fq 'set position of item "ACECode.app"' "$dmg_script"
grep -Fq 'set position of item "Applications"' "$dmg_script"
grep -Fq '/bin/ln -s /Applications "$staging_root/Applications"' "$dmg_script"
grep -Fq 'readlink "$staging_root/Applications"' "$dmg_script"
grep -Fq '/usr/bin/hdiutil convert' "$dmg_script"
grep -Fq -- '-format UDZO' "$dmg_script"
grep -Fq '.VolumeIcon.icns' "$dmg_script"

if grep -Fq 'Install ACECode.app' "$dmg_script"; then
    echo "DMG helper must not package the obsolete custom installer" >&2
    exit 1
fi
if grep -Eq 'README|instructions_path|--instructions' "$dmg_script"; then
    echo "DMG helper must not package visible installation instructions" >&2
    exit 1
fi
if grep -Eq 'set position of item "(Install ACECode\.app|README)' "$dmg_script"; then
    echo "Finder layout must contain only the app and Applications destination" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    background_png="$temporary_root/ACECode-DMG.png"
    /usr/bin/sips -s format png "$dmg_background" \
        --out "$background_png" >/dev/null
    test -s "$background_png"
    /usr/bin/sips -g pixelWidth -g pixelHeight "$background_png" \
        >"$temporary_root/background-size.txt"
    grep -Fq 'pixelWidth: 660' "$temporary_root/background-size.txt"
    grep -Fq 'pixelHeight: 400' "$temporary_root/background-size.txt"
fi

echo "macOS DMG layout contract checks passed"
