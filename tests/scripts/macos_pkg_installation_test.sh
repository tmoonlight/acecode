#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pkg_script="$repo_root/scripts/macos_create_pkg.sh"
notarize_pkg_script="$repo_root/scripts/macos_notarize_pkg.sh"

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-pkg-installation-test.XXXXXX")"
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

bash -n "$pkg_script"
expect_status 0 "PKG help" bash "$pkg_script" --help
expect_status 2 "unknown PKG argument" bash "$pkg_script" --unknown

grep -Fq '/usr/bin/pkgbuild' "$pkg_script"
grep -Fq '/usr/bin/productbuild' "$pkg_script"
grep -Fq -- '--install-location /Applications' "$pkg_script"
grep -Fq 'enable_anywhere="false"' "$pkg_script"
grep -Fq 'enable_currentUserHome="true"' "$pkg_script"
grep -Fq 'enable_localSystem="false"' "$pkg_script"
grep -Fq 'require-scripts="false"' "$pkg_script"
grep -Fq '<must-close>' "$pkg_script"
grep -Fq '<app id="dev.acecode.desktop"/>' "$pkg_script"
grep -Fq '/usr/sbin/installer -dominfo' "$pkg_script"
grep -Fq 'relocatable="false"' "$pkg_script"
grep -Fq 'Refusing to overwrite existing Installer package output' "$pkg_script"

if grep -Eq 'Authorization(Create|CopyRights)|[[:space:]]sudo[[:space:]]' "$pkg_script"; then
    echo "Current-user PKG helper must not request administrator privileges" >&2
    exit 1
fi
if grep -Fq 'enable_localSystem="true"' "$pkg_script" ||
   grep -Fq 'enable_anywhere="true"' "$pkg_script"; then
    echo "Current-user PKG helper must not enable system or arbitrary-volume installation" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    expect_status 2 "missing PKG arguments" bash "$pkg_script"

    fake_app="$temporary_root/ACECode.app"
    fake_exec="$fake_app/Contents/MacOS/ACECode"
    info_plist="$fake_app/Contents/Info.plist"
    models_dev_dir="$fake_app/Contents/Resources/share/acecode/models_dev"
    mkdir -p "$(dirname "$fake_exec")"
    mkdir -p "$models_dev_dir"
    printf '%s\n' '{}' > "$models_dev_dir/api.json"
    printf '%s\n' '{}' > "$models_dev_dir/MANIFEST.json"
    printf '%s\n' 'MIT' > "$models_dev_dir/LICENSE"
    printf '%s\n' 'int main(void) { return 0; }' > "$temporary_root/main.c"
    xcrun clang -arch "$(uname -m)" "$temporary_root/main.c" -o "$fake_exec"

    /usr/bin/plutil -create xml1 "$info_plist"
    /usr/bin/plutil -insert CFBundleIdentifier -string dev.acecode.desktop "$info_plist"
    /usr/bin/plutil -insert CFBundleShortVersionString -string 9.8.7 "$info_plist"
    /usr/bin/plutil -insert CFBundleVersion -string 987 "$info_plist"
    /usr/bin/plutil -insert CFBundleExecutable -string ACECode "$info_plist"
    /usr/bin/plutil -insert CFBundlePackageType -string APPL "$info_plist"
    /usr/bin/plutil -insert LSMinimumSystemVersion -string 11.0 "$info_plist"

    output_pkg="$temporary_root/ACECode-test-unsigned.pkg"
    bash "$pkg_script" --app "$fake_app" --output "$output_pkg"
    test -s "$output_pkg"
    expect_status 1 "existing PKG output" \
        bash "$pkg_script" --app "$fake_app" --output "$output_pkg"
    expect_status 1 "unsigned PKG notarization rejection" \
        bash "$notarize_pkg_script" --pkg "$output_pkg"

    domain_info="$temporary_root/domains.plist"
    /usr/sbin/installer -dominfo -pkg "$output_pkg" -plist > "$domain_info"
    [[ "$(grep -Fc '<key>Domain</key>' "$domain_info")" -eq 1 ]]
    [[ "$(grep -Fc '<string>CurrentUserHomeDirectory</string>' "$domain_info")" -eq 1 ]]
    ! grep -Eq '<string>(LocalSystem|NetworkDomain)</string>' "$domain_info"

    expanded="$temporary_root/expanded"
    /usr/sbin/pkgutil --expand-full "$output_pkg" "$expanded"
    distribution="$expanded/Distribution"
    test -f "$distribution"
    grep -Fq 'enable_anywhere="false"' "$distribution"
    grep -Fq 'enable_currentUserHome="true"' "$distribution"
    grep -Fq 'enable_localSystem="false"' "$distribution"
    grep -Fq '<must-close>' "$distribution"
    ! grep -Fq 'customLocation=' "$distribution"
    ! grep -Fq '<relocate>' "$distribution"

    package_info="$(find "$expanded" -type f -name PackageInfo -print -quit)"
    test -n "$package_info"
    grep -Fq 'install-location="/Applications"' "$package_info"
    grep -Fq 'relocatable="false"' "$package_info"
    test -d "$(find "$expanded" -type d -name ACECode.app -print -quit)"
    expanded_models_dev_dir="$(find "$expanded" -type d \
        -path '*/ACECode.app/Contents/Resources/share/acecode/models_dev' \
        -print -quit)"
    test -n "$expanded_models_dev_dir"
    for models_dev_file in api.json MANIFEST.json LICENSE; do
        test -f "$expanded_models_dev_dir/$models_dev_file"
    done
    ! find "$expanded" -type d -name Scripts -print -quit | grep -q .
fi

echo "macOS current-user PKG installation checks passed"
