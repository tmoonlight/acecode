#!/usr/bin/env bash
# Production adaptation of the v6 local installer build; never signs the payload.
set -euo pipefail
# shellcheck source=scripts/macos_stapler.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)/scripts/macos_stapler.sh"
usage() {
    printf 'Usage: bash packaging/macos-installer/build.sh --app ACECode.app --output NEW-DIR --arch x64|arm64\n'
}
source_app=""; output=""; arch=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --app|--output|--arch)
            [[ $# -ge 2 && -n "$2" ]] || { usage >&2; exit 2; }
            case "$1" in --app) source_app="$2";; --output) output="$2";; --arch) arch="$2";; esac
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$arch" in x64) target=x86_64;; arm64) target=arm64;; *) usage >&2; exit 2;; esac
[[ -n "$source_app" && -n "$output" ]] || { usage >&2; exit 2; }
[[ "$(uname -s)" == Darwin ]] || { printf 'Darwin required\n' >&2; exit 1; }
[[ -d "$source_app" && "$(basename "$source_app")" == ACECode.app && ! -e "$output" && ! -L "$output" ]] || {
    printf 'Source missing or output already exists\n' >&2; exit 1;
}
source_app="$(cd "$source_app" && pwd -P)"
source_dir="$(cd "$(dirname "$0")" && pwd)"
info="$source_app/Contents/Info.plist"
[[ "$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "$info")" == dev.acecode.desktop ]]
version="$(/usr/bin/plutil -extract CFBundleShortVersionString raw -o - "$info")"
build_version="$(/usr/bin/plutil -extract CFBundleVersion raw -o - "$info")"
for binary in ACECode acecode-daemon; do
    /usr/bin/lipo "$source_app/Contents/MacOS/$binary" -verify_arch "$target"
done
/usr/bin/codesign --verify --deep --strict "$source_app"
details="$(/usr/bin/codesign -dv --verbose=4 "$source_app" 2>&1)"
grep -Fxq 'TeamIdentifier=T52GZCH73Y' <<< "$details"
grep -Fq 'Authority=Developer ID Application:' <<< "$details"
acecode_stapler validate "$source_app"
/usr/sbin/spctl --assess --type execute "$source_app"
mkdir -p "$output"
output="$(cd "$output" && pwd -P)"
app="$output/ACECode Installer.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
xcrun swiftc -swift-version 5 -O -target "${target}-apple-macosx11.0" \
    -sdk "$(xcrun --sdk macosx --show-sdk-path)" -framework AppKit \
    "$source_dir/Engine.swift" "$source_dir/main.swift" -o "$app/Contents/MacOS/ACECodeInstaller"
/usr/bin/lipo "$app/Contents/MacOS/ACECodeInstaller" -verify_arch "$target"
plist="$app/Contents/Info.plist"
/usr/bin/plutil -create xml1 "$plist"
/usr/bin/plutil -insert CFBundleIdentifier -string dev.acecode.userinstaller "$plist"
/usr/bin/plutil -insert CFBundleExecutable -string ACECodeInstaller "$plist"
/usr/bin/plutil -insert CFBundleName -string 'ACECode Installer' "$plist"
/usr/bin/plutil -insert CFBundlePackageType -string APPL "$plist"
/usr/bin/plutil -insert CFBundleVersion -string "$build_version" "$plist"
/usr/bin/plutil -insert CFBundleShortVersionString -string "$version" "$plist"
/usr/bin/plutil -insert LSMinimumSystemVersion -string 11.0 "$plist"
icon_name="$(/usr/bin/plutil -extract CFBundleIconFile raw -o - "$info")"
[[ -n "$icon_name" && "$icon_name" != */* && "$icon_name" != .* ]] || { printf 'Invalid app icon name\n' >&2; exit 1; }
[[ "$icon_name" == *.icns ]] || icon_name="$icon_name.icns"
/usr/bin/ditto "$source_app/Contents/Resources/$icon_name" "$app/Contents/Resources/InstallerIcon.icns"
/usr/bin/plutil -insert CFBundleIconFile -string InstallerIcon.icns "$plist"
/usr/bin/ditto "$source_app" "$app/Contents/Resources/ACECode.app"
/usr/bin/diff -rq "$source_app" "$app/Contents/Resources/ACECode.app"
printf 'Unsigned installer wrapper (trusted payload unchanged): %s\n' "$app"
