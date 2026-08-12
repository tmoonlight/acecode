#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dmg_script="$repo_root/scripts/macos_create_dmg.sh"
desktop_cmake="$repo_root/cmake/acecode_desktop.cmake"
user_applications_plist="$repo_root/cmake/macos/ACECodeUserApplicationsInfo.plist.in"
user_applications_source="$repo_root/src/macos_user_applications/main.mm"
dmg_background="$repo_root/assets/macos/acecode-dmg-background.svg"
system_applications_icon="/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/ApplicationsFolderIcon.icns"

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
grep -Fq 'ApplicationsFolderIcon.icns' "$desktop_cmake"
grep -Fq 'Missing macOS Applications folder icon' "$desktop_cmake"
grep -Fq 'add_executable(acecode-user-applications MACOSX_BUNDLE' "$desktop_cmake"
grep -Fq 'RUNTIME_OUTPUT_NAME "Applications"' "$desktop_cmake"
grep -Fq 'MACOSX_PACKAGE_LOCATION "Resources"' "$desktop_cmake"
grep -Fq '<key>CFBundleDocumentTypes</key>' "$user_applications_plist"
grep -Fq '<string>com.apple.application-bundle</string>' "$user_applications_plist"
grep -Fq '<string>Alternate</string>' "$user_applications_plist"
grep -Fq 'application:(NSApplication*)application' "$user_applications_source"
grep -Fq 'openURLs:(NSArray<NSURL*>*)urls' "$user_applications_source"
grep -Fq 'dropped_source_is_expected' "$user_applications_source"
grep -Fq 'expected_source_url' "$user_applications_source"
grep -Fq 'NSHomeDirectory()' "$user_applications_source"

grep -Fq -- '-format UDRW' "$dmg_script"
grep -Fq '/usr/bin/osascript' "$dmg_script"
grep -Fq 'set background picture of iconOptions' "$dmg_script"
grep -Fq 'set position of item "ACECode.app"' "$dmg_script"
grep -Fq 'set position of item "Applications.app"' "$dmg_script"
grep -Fq '/usr/bin/ditto "$user_applications_path" "$staging_root/Applications.app"' "$dmg_script"
grep -Fq 'DMG must not contain a system Applications link.' "$dmg_script"
grep -Fq '/usr/bin/hdiutil convert' "$dmg_script"
grep -Fq -- '-format UDZO' "$dmg_script"
grep -Fq '.VolumeIcon.icns' "$dmg_script"

if grep -Eq 'ln[[:space:]].*/Applications|ln[[:space:]]+-s[[:space:]]+/Applications' "$dmg_script"; then
    echo "DMG helper must not create a system /Applications link" >&2
    exit 1
fi
if grep -Eq 'README|instructions_path|--instructions' "$dmg_script"; then
    echo "DMG helper must not package visible installation instructions" >&2
    exit 1
fi
if [[ "$(grep -Fc 'set position of item "' "$dmg_script")" -ne 2 ]]; then
    echo "Finder layout must contain only ACECode.app and Applications.app" >&2
    exit 1
fi
if grep -Eq 'Authorization(Create|CopyRights)|[[:space:]]sudo[[:space:]]' "$user_applications_source"; then
    echo "Current-user Applications target must not request administrator privileges" >&2
    exit 1
fi
if grep -Fq '@"/Applications"' "$user_applications_source"; then
    echo "Current-user Applications target must not reference system /Applications" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    test -s "$system_applications_icon"

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
