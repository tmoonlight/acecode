#!/usr/bin/env bash
set -euo pipefail
set +x
# shellcheck source=scripts/macos_stapler.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/macos_stapler.sh"
usage() {
    printf 'Usage: bash scripts/macos_create_installer_dmg.sh --app ACECode.app --output NEW.dmg --arch x64|arm64 --identity APPLICATION-IDENTITY [--keychain PATH] [--keychain-profile NAME]\n'
}
app=""; output=""; arch=""; identity=""; keychain=""; profile="${NOTARYTOOL_PROFILE:-}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --app|--output|--arch|--identity|--keychain|--keychain-profile)
            [[ $# -ge 2 && -n "$2" ]] || { usage >&2; exit 2; }
            case "$1" in
                --app) app="$2";; --output) output="$2";; --arch) arch="$2";;
                --identity) identity="$2";; --keychain) keychain="$2";; --keychain-profile) profile="$2";;
            esac
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
[[ -n "$app" && "$output" == *.dmg && -n "$identity" && "$identity" != - ]] || { usage >&2; exit 2; }
case "$arch" in x64|arm64) ;; *) usage >&2; exit 2;; esac
[[ ! -e "$output" && ! -L "$output" ]] || { printf 'Refusing existing output\n' >&2; exit 1; }
[[ "$(uname -s)" == Darwin ]] || { printf 'Darwin required\n' >&2; exit 1; }
auth=()
if [[ -n "$profile" ]]; then
    auth=(--keychain-profile "$profile")
else
    [[ -n "${APPLE_ID:-}" && -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ]] || {
        printf 'Notarization credentials required\n' >&2; exit 2;
    }
    auth=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_SPECIFIC_PASSWORD")
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/acecode-installer-release.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
bash "$root/packaging/macos-installer/build.sh" --app "$app" --output "$work/image" --arch "$arch"
installer="$work/image/ACECode Installer.app"
sign_args=(--sign "$identity" --timestamp)
if [[ -n "$keychain" ]]; then sign_args+=(--keychain "$keychain"); fi
# Deliberately NOT --deep: only sign the wrapper, never the stapled payload.
/usr/bin/codesign --force --options runtime "${sign_args[@]}" "$installer"
/usr/bin/codesign --verify --deep --strict "$installer"
details="$(/usr/bin/codesign -dv --verbose=4 "$installer" 2>&1)"
grep -Fxq 'TeamIdentifier=T52GZCH73Y' <<< "$details"
grep -Fq 'Authority=Developer ID Application:' <<< "$details"
/usr/bin/diff -rq "$app" "$installer/Contents/Resources/ACECode.app"
notarize() {
    local input="$1" ticket="$2"
    xcrun notarytool submit "$input" "${auth[@]}" --wait --output-format json > "$work/result.json"
    local status submission
    status="$(/usr/bin/plutil -extract status raw -o - "$work/result.json")"
    if [[ "$status" != Accepted ]]; then
        submission="$(/usr/bin/plutil -extract id raw -o - "$work/result.json" 2>/dev/null || true)"
        if [[ -n "$submission" ]]; then xcrun notarytool log "$submission" "${auth[@]}" || true; fi
        printf 'Notarization rejected: %s\n' "$status" >&2; exit 1
    fi
    acecode_stapler staple "$ticket"
    acecode_stapler validate "$ticket"
}
/usr/bin/ditto -c -k --keepParent "$installer" "$work/installer.zip"
notarize "$work/installer.zip" "$installer"
/usr/bin/codesign --verify --deep --strict "$installer"
/usr/sbin/spctl --assess --type execute --verbose=4 "$installer"
/usr/bin/diff -rq "$app" "$installer/Contents/Resources/ACECode.app"
acecode_stapler validate "$installer/Contents/Resources/ACECode.app"
# The image root contains only the installer, not a misleading Applications link.
# DiskImages can transiently fail with EBUSY on hosted macOS runners. Keep each
# attempt separate, capture logs without a pipe, and retry only that error.
for attempt in 1 2 3; do
    attempt_image="$work/release-$attempt.dmg"
    if /usr/bin/hdiutil create -volname 'ACECode Installer' -srcfolder "$work/image" \
        -fs HFS+ -format UDZO "$attempt_image" > "$work/hdiutil.log" 2>&1; then
        cat "$work/hdiutil.log"
        mv "$attempt_image" "$work/release.dmg"
        break
    else
        status=$?
        cat "$work/hdiutil.log" >&2
        if [[ "$attempt" -eq 3 ]] || ! grep -Fq 'Resource busy' "$work/hdiutil.log"; then
            exit "$status"
        fi
        printf 'Disk image creation is busy; retrying (%s/3)\n' "$attempt" >&2
        sleep "$attempt"
    fi
done
/usr/bin/codesign "${sign_args[@]}" "$work/release.dmg"
notarize "$work/release.dmg" "$work/release.dmg"
/usr/bin/codesign --verify --strict "$work/release.dmg"
/usr/sbin/spctl --assess --type open --context context:primary-signature --verbose=4 "$work/release.dmg"
mkdir -p "$(dirname "$output")"
/usr/bin/ditto "$work/release.dmg" "$output"
printf 'Signed, notarized and stapled installer DMG: %s\n' "$output"
