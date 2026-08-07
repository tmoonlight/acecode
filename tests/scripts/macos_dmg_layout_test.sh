#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dmg_script="$repo_root/scripts/macos_create_dmg.sh"
icon_script="$repo_root/scripts/macos_generate_icns.sh"
desktop_cmake="$repo_root/cmake/acecode_desktop.cmake"
installer_plist="$repo_root/cmake/macos/ACECodeUserInstallerInfo.plist.in"
installer_source="$repo_root/src/macos_installer/main.mm"
installer_icon="$repo_root/assets/macos/acecode-installer.svg"
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
bash -n "$icon_script"
expect_status 0 "DMG help" bash "$dmg_script" --help
expect_status 0 "ICNS help" bash "$icon_script" --help
expect_status 2 "unknown DMG argument" bash "$dmg_script" --unknown
expect_status 2 "unknown ICNS argument" bash "$icon_script" --unknown

test -f "$installer_icon"
test -f "$dmg_background"
grep -Fq 'acecode-installer.icns' "$desktop_cmake"
grep -Fq 'macos_generate_icns.sh' "$desktop_cmake"
grep -Fq 'MACOSX_PACKAGE_LOCATION "Resources"' "$desktop_cmake"
grep -Fq '<key>CFBundleDocumentTypes</key>' "$installer_plist"
grep -Fq '<string>com.apple.application-bundle</string>' "$installer_plist"
grep -Fq '<string>Alternate</string>' "$installer_plist"
grep -Fq 'application:(NSApplication*)application' "$installer_source"
grep -Fq 'openURLs:(NSArray<NSURL*>*)urls' "$installer_source"
grep -Fq 'dropped_source_is_expected' "$installer_source"
grep -Fq 'expected_source_url' "$installer_source"

grep -Fq -- '-format UDRW' "$dmg_script"
grep -Fq '/usr/bin/osascript' "$dmg_script"
grep -Fq 'set background picture of iconOptions' "$dmg_script"
grep -Fq 'set position of item "ACECode.app"' "$dmg_script"
grep -Fq 'set position of item "Install ACECode.app"' "$dmg_script"
grep -Fq '/usr/bin/hdiutil convert' "$dmg_script"
grep -Fq -- '-format UDZO' "$dmg_script"
grep -Fq '.VolumeIcon.icns' "$dmg_script"
grep -Fq 'Refusing to package a system /Applications link.' "$dmg_script"

if grep -Eq 'ln[[:space:]].*/Applications|ln[[:space:]]+-s[[:space:]]+/Applications' "$dmg_script"; then
    echo "DMG helper must not create a system /Applications link" >&2
    exit 1
fi
if grep -Eq 'Authorization(Create|CopyRights)|[[:space:]]sudo[[:space:]]' "$installer_source"; then
    echo "Installer must not request administrator privileges" >&2
    exit 1
fi
if grep -Fq '@"/Applications"' "$installer_source"; then
    echo "Installer must not target system /Applications" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    generated_icon="$temporary_root/acecode-installer.icns"
    expect_status 0 "generate installer icon" \
        bash "$icon_script" --source "$installer_icon" --output "$generated_icon"
    test -s "$generated_icon"

    /usr/bin/iconutil -c iconset -o "$temporary_root/icon.iconset" \
        "$generated_icon"
    test "$(find "$temporary_root/icon.iconset" -type f -name '*.png' | wc -l | tr -d ' ')" -eq 10

    background_png="$temporary_root/ACECode-DMG.png"
    /usr/bin/sips -s format png "$dmg_background" \
        --out "$background_png" >/dev/null
    test -s "$background_png"
    /usr/bin/sips -g pixelWidth -g pixelHeight "$background_png" \
        >"$temporary_root/background-size.txt"
    grep -Fq 'pixelWidth: 720' "$temporary_root/background-size.txt"
    grep -Fq 'pixelHeight: 520' "$temporary_root/background-size.txt"
fi

echo "macOS DMG layout contract checks passed"
