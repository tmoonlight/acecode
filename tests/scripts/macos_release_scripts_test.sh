#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sign_script="$repo_root/scripts/macos_codesign.sh"
dmg_script="$repo_root/scripts/macos_create_dmg.sh"
notarize_script="$repo_root/scripts/macos_notarize.sh"
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

for script in "$sign_script" "$dmg_script" "$notarize_script"; do
    bash -n "$script"
    expect_status 0 "help for $(basename "$script")" bash "$script" --help
    expect_status 2 "unknown argument for $(basename "$script")" \
        bash "$script" --definitely-unknown
done

grep -Fq -- '--bundle <path>' "$sign_script"
grep -Fq -- '--keychain-profile <name>' "$notarize_script"
grep -Fq 'Install ACECode.app' "$dmg_script"
grep -Fq 'identity_fingerprint=' "$package_workflow"
grep -Fq 'echo "identity=$identity_fingerprint"' "$package_workflow"
grep -Fq 'security list-keychains -d user -s' "$package_workflow"

if grep -Fq 'identity="$MACOS_CODESIGN_IDENTITY"' "$package_workflow"; then
    echo "macOS signing must use the imported identity fingerprint, not a configured name" >&2
    exit 1
fi

if grep -Eq 'ln[[:space:]].*/Applications|ln[[:space:]]+-s[[:space:]]+/Applications' "$dmg_script"; then
    echo "DMG helper must not create a system /Applications link" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    expect_status 2 "missing DMG arguments" bash "$dmg_script"
    expect_status 2 "missing notarization file" bash "$notarize_script"
    expect_status 2 "missing signing identity" bash "$sign_script"

    touch "$temporary_root/unsigned.dmg"
    expect_status 2 "missing notarization credentials" \
        env -u NOTARYTOOL_PROFILE -u APPLE_ID -u APPLE_TEAM_ID \
            -u APPLE_APP_SPECIFIC_PASSWORD \
            bash "$notarize_script" --file "$temporary_root/unsigned.dmg"
fi

echo "macOS release script contract checks passed"
