#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sign_script="$repo_root/scripts/macos_codesign.sh"
pkg_script="$repo_root/scripts/macos_create_pkg.sh"
notarize_pkg_script="$repo_root/scripts/macos_notarize_pkg.sh"
notarize_app_script="$repo_root/scripts/macos_notarize_app.sh"
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

for script in "$sign_script" "$pkg_script" "$notarize_pkg_script" \
              "$notarize_app_script" "$update_zip_script"; do
    bash -n "$script"
    expect_status 0 "help for $(basename "$script")" bash "$script" --help
    expect_status 2 "unknown argument for $(basename "$script")" \
        bash "$script" --definitely-unknown
done

grep -Fq -- '--keychain-profile <name>' "$notarize_pkg_script"
grep -Fq -- '--keychain-profile <name>' "$notarize_app_script"
grep -Fq -- '--installer-identity <Developer ID Installer identity>' "$pkg_script"
grep -Fq '/usr/sbin/pkgutil --check-signature' "$notarize_pkg_script"
grep -Fq "grep -Fq 'Developer ID Installer:'" "$notarize_pkg_script"
grep -Fq -- '--type install' "$notarize_pkg_script"
grep -Fq -- '--require-trusted' "$update_zip_script"
grep -Fq -- '/usr/bin/ditto -c -k --keepParent' "$update_zip_script"
grep -Fq 'Contents/Resources/share/acecode/models_dev' "$update_zip_script"
grep -Fq 'share/acecode/models_dev' "$pkg_script"
grep -Fq 'identity_fingerprint=' "$package_workflow"
grep -Fq 'installer_identity_name=' "$package_workflow"
grep -Fq 'echo "identity=$identity_fingerprint"' "$package_workflow"
grep -Fq 'echo "installer_identity=$installer_identity_name"' "$package_workflow"
grep -Fq 'security find-identity -v -p basic "$keychain_path"' "$package_workflow"
grep -Fq 'Developer ID Application identity does not match APPLE_TEAM_ID' "$package_workflow"
grep -Fq 'Developer ID Installer identity does not match APPLE_TEAM_ID' "$package_workflow"
grep -Fq 'security list-keychains -d user -s' "$package_workflow"
grep -Fq 'MACOS_INSTALLER_CERTIFICATE_BASE64' "$package_workflow"
grep -Fq 'MACOS_INSTALLER_CERTIFICATE_PASSWORD' "$package_workflow"
grep -Fq 'echo "pkg_enabled=true"' "$package_workflow"
grep -Fq 'echo "pkg_enabled=false"' "$package_workflow"
grep -Fq 'macOS Installer credentials must configure both' "$package_workflow"
grep -Fq -- '-T /usr/bin/pkgbuild' "$package_workflow"
grep -Fq -- '-T /usr/bin/productbuild' "$package_workflow"
grep -Fq 'scripts/macos_notarize_app.sh --app "build/ACECode.app"' "$package_workflow"
grep -Fq 'scripts/macos_create_update_zip.sh' "$package_workflow"
grep -Fq 'scripts/macos_create_pkg.sh' "$package_workflow"
grep -Fq 'scripts/macos_notarize_pkg.sh' "$package_workflow"
grep -Fq -- '--installer-identity "${{ steps.macos-keychain.outputs.installer_identity }}"' "$package_workflow"
grep -Fq 'acecode-${{ matrix.id }}-pkg' "$package_workflow"
grep -Fq 'DMG artifacts are no longer permitted in tagged releases' "$package_workflow"
grep -Fq 'Unsigned PKG artifacts must not be published' "$package_workflow"
grep -Fq 'Unsigned macOS update artifacts must not be published' "$package_workflow"
grep -Fq 'Tagged releases allow either zero or two signed macOS PKGs' "$package_workflow"
grep -Fq 'ACECode-${release_version}-macos-${arch}.pkg' "$package_workflow"
grep -Fq 'acecode-${{ matrix.id }}-update' "$package_workflow"
grep -Fq 'Tagged macOS application releases require secrets:' "$package_workflow"
grep -Fq 'trust_args+=(--require-trusted)' "$package_workflow"
grep -Fq "steps.macos-release.outputs.pkg_enabled == 'true'" "$package_workflow"

if grep -Fq 'pkg_path="ACECode-${{ steps.package-version.outputs.version }}-${{ matrix.id }}${unsigned_suffix}.pkg"' "$package_workflow"; then
    echo "GitHub Actions must not create unsigned PKG artifacts" >&2
    exit 1
fi

if grep -Fq 'identity="$MACOS_CODESIGN_IDENTITY"' "$package_workflow"; then
    echo "macOS app signing must use the imported identity fingerprint" >&2
    exit 1
fi

if grep -ERni \
    --exclude='macos_create_pkg.sh' \
    'macos_create_dmg|macos_notarize\.sh|macos-dmg' \
    "$package_workflow" "$repo_root/scripts"; then
    echo "Active macOS release automation must not retain DMG packaging" >&2
    exit 1
fi
if grep -Fq -- "-o -name '*.dmg'" "$package_workflow"; then
    echo "Tagged release asset collection must not include DMG files" >&2
    exit 1
fi
if grep -En 'Applications\.app|acecode-user-applications|macos_verify_user_applications_drop' \
    "$package_workflow"; then
    echo "Release workflow must not build or package the fake Applications target" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    expect_status 2 "missing PKG arguments" bash "$pkg_script"
    expect_status 2 "missing PKG notarization input" bash "$notarize_pkg_script"
    expect_status 2 "missing app notarization input" bash "$notarize_app_script"
    expect_status 2 "missing update ZIP arguments" bash "$update_zip_script"
    expect_status 2 "missing signing identity" bash "$sign_script"

    fake_app="$temporary_root/ACECode.app"
    mkdir -p "$fake_app/Contents/MacOS"
    mkdir -p "$fake_app/Contents/Resources/share/acecode/models_dev"
    touch "$fake_app/Contents/MacOS/ACECode" \
          "$fake_app/Contents/MacOS/acecode-daemon"
    printf '%s\n' '{}' > \
        "$fake_app/Contents/Resources/share/acecode/models_dev/api.json"
    printf '%s\n' '{}' > \
        "$fake_app/Contents/Resources/share/acecode/models_dev/MANIFEST.json"
    printf '%s\n' 'MIT' > \
        "$fake_app/Contents/Resources/share/acecode/models_dev/LICENSE"
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
    for models_dev_file in api.json MANIFEST.json LICENSE; do
        test -f "$temporary_root/extracted-update/ACECode.app/Contents/Resources/share/acecode/models_dev/$models_dev_file"
        test -f "$temporary_root/extracted-update/share/acecode/models_dev/$models_dev_file"
    done
fi

echo "macOS release script contract checks passed"
