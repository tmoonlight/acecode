#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sign_script="$repo_root/scripts/macos_codesign.sh"
dmg_script="$repo_root/scripts/macos_create_dmg.sh"
notarize_script="$repo_root/scripts/macos_notarize.sh"
notarize_app_script="$repo_root/scripts/macos_notarize_app.sh"
drop_verify_script="$repo_root/scripts/macos_verify_user_applications_drop.sh"
update_zip_script="$repo_root/scripts/macos_create_update_zip.sh"
package_workflow="$repo_root/.github/workflows/package.yml"

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-release-scripts.XXXXXX")"
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

for script in "$sign_script" "$dmg_script" "$notarize_script" \
              "$notarize_app_script" "$drop_verify_script" \
              "$update_zip_script"; do
    bash -n "$script"
    expect_status 0 "help for $(basename "$script")" bash "$script" --help
    expect_status 2 "unknown argument for $(basename "$script")" \
        bash "$script" --definitely-unknown
done

grep -Fq -- '--bundle <path>' "$sign_script"
grep -Fq -- '--keychain-profile <name>' "$notarize_script"
grep -Fq -- '--keychain-profile <name>' "$notarize_app_script"
grep -Fq -- '--require-trusted' "$update_zip_script"
grep -Fq -- '/usr/bin/ditto -c -k --keepParent' "$update_zip_script"
grep -Fq -- '--user-applications <Applications.app>' "$dmg_script"
grep -Fq '/usr/bin/ditto "$user_applications_path" "$staging_root/Applications.app"' "$dmg_script"
grep -Fq 'detach_with_retry "$detach_target"' "$dmg_script"
grep -Fq '/usr/bin/hdiutil detach "$target" -force' "$dmg_script"
grep -Fq 'identity_fingerprint=' "$package_workflow"
grep -Fq 'echo "identity=$identity_fingerprint"' "$package_workflow"
grep -Fq 'security list-keychains -d user -s' "$package_workflow"
grep -Fq 'scripts/macos_notarize_app.sh --app "build/ACECode.app"' "$package_workflow"
grep -Fq 'scripts/macos_create_update_zip.sh' "$package_workflow"
grep -Fq 'scripts/macos_create_dmg.sh' "$package_workflow"
grep -Fq 'cmake --build build --config MinSizeRel --target acecode-user-applications' "$package_workflow"
grep -Fq 'bash scripts/macos_verify_user_applications_drop.sh' "$package_workflow"
grep -Fq -- '--bundle "build/Applications.app"' "$package_workflow"
grep -Fq -- '--user-applications "build/Applications.app"' "$package_workflow"
grep -Fq 'acecode-${{ matrix.id }}-update' "$package_workflow"
grep -Fq 'Tagged macOS releases require secrets:' "$package_workflow"
grep -Fq 'trust_args+=(--require-trusted)' "$package_workflow"

if grep -Fq 'identity="$MACOS_CODESIGN_IDENTITY"' "$package_workflow"; then
    echo "macOS signing must use the imported identity fingerprint, not a configured name" >&2
    exit 1
fi

if grep -Eq 'ln[[:space:]].*/Applications|ln[[:space:]]+-s[[:space:]]+/Applications' "$dmg_script"; then
    echo "DMG helper must not create a system /Applications link" >&2
    exit 1
fi

if grep -Eq 'Install ACECode\.app|README 安装说明|--instructions' "$dmg_script"; then
    echo "DMG helper must expose only the app and current-user Applications target" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    expect_status 2 "missing DMG arguments" bash "$dmg_script"
    expect_status 2 "missing notarization file" bash "$notarize_script"
    expect_status 2 "missing app notarization input" bash "$notarize_app_script"
    expect_status 2 "missing update ZIP arguments" bash "$update_zip_script"
    expect_status 2 "missing signing identity" bash "$sign_script"

    touch "$temporary_root/unsigned.dmg"
    expect_status 2 "missing notarization credentials" \
        env -u NOTARYTOOL_PROFILE -u APPLE_ID -u APPLE_TEAM_ID \
            -u APPLE_APP_SPECIFIC_PASSWORD \
            bash "$notarize_script" --file "$temporary_root/unsigned.dmg"

    fake_app="$temporary_root/ACECode.app"
    mkdir -p "$fake_app/Contents/MacOS"
    touch "$fake_app/Contents/MacOS/ACECode" \
          "$fake_app/Contents/MacOS/acecode-daemon"
    chmod +x "$fake_app/Contents/MacOS/ACECode" \
             "$fake_app/Contents/MacOS/acecode-daemon"
    expect_status 2 "missing app notarization credentials" \
        env -u NOTARYTOOL_PROFILE -u APPLE_ID -u APPLE_TEAM_ID \
            -u APPLE_APP_SPECIFIC_PASSWORD \
            bash "$notarize_app_script" --app "$fake_app"
    expect_status 0 "create structural update ZIP" \
        bash "$update_zip_script" \
            --app "$fake_app" \
            --output "$temporary_root/ACECode-test-update.zip"
    test -f "$temporary_root/ACECode-test-update.zip"
    mkdir -p "$temporary_root/extracted-update"
    /usr/bin/ditto -x -k "$temporary_root/ACECode-test-update.zip" \
        "$temporary_root/extracted-update"
    test -x "$temporary_root/extracted-update/ACECode.app/Contents/MacOS/ACECode"
    test -x "$temporary_root/extracted-update/ACECode.app/Contents/MacOS/acecode-daemon"
    test -x "$temporary_root/extracted-update/acecode"
fi

echo "macOS release script contract checks passed"
