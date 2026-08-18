#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_create_pkg.sh --app <ACECode.app> --output <ACECode.pkg> \
      [--installer-identity <Developer ID Installer identity>] \
      [--keychain <path>]

Creates a macOS Installer product archive that installs ACECode only into the
current user's ~/Applications directory. The package contains no install
scripts, disables system and arbitrary-volume installation domains, and marks
ACECode as an application that must be closed before replacement.

Pass --installer-identity for a signed release package. --keychain optionally
limits productbuild to a specific keychain. Omitting the identity creates an
unsigned package for local structural inspection only.
USAGE
}

app_path=""
output_path=""
installer_identity=""
keychain=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --app" >&2
                exit 2
            fi
            app_path="$2"
            shift 2
            ;;
        --output)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --output" >&2
                exit 2
            fi
            output_path="$2"
            shift 2
            ;;
        --installer-identity)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --installer-identity" >&2
                exit 2
            fi
            installer_identity="$2"
            shift 2
            ;;
        --keychain)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --keychain" >&2
                exit 2
            fi
            keychain="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS PKG creation must run on Darwin." >&2
    exit 1
fi
if [[ -z "$app_path" || -z "$output_path" ]]; then
    echo "--app and --output are required." >&2
    usage >&2
    exit 2
fi
if [[ ! -d "$app_path" || -L "$app_path" ||
      "$(basename "$app_path")" != "ACECode.app" ]]; then
    echo "Missing or unsafe ACECode.app package payload: $app_path" >&2
    exit 1
fi
if [[ "$output_path" != *.pkg ]]; then
    echo "Installer package output must end in .pkg: $output_path" >&2
    exit 2
fi
if [[ -n "$keychain" && -z "$installer_identity" ]]; then
    echo "--keychain requires --installer-identity." >&2
    exit 2
fi
if [[ -n "$keychain" && ! -f "$keychain" ]]; then
    echo "Missing signing keychain: $keychain" >&2
    exit 1
fi

output_dir="$(dirname "$output_path")"
if [[ ! -d "$output_dir" ]]; then
    echo "Installer package output directory does not exist: $output_dir" >&2
    exit 1
fi
if [[ -L "$output_dir" ]]; then
    echo "Installer package output directory must not be a symlink: $output_dir" >&2
    exit 1
fi
if [[ -e "$output_path" || -L "$output_path" ]]; then
    echo "Refusing to overwrite existing Installer package output: $output_path" >&2
    exit 1
fi

info_plist="$app_path/Contents/Info.plist"
if [[ ! -f "$info_plist" ]]; then
    echo "Missing ACECode Info.plist: $info_plist" >&2
    exit 1
fi
models_dev_dir="$app_path/Contents/Resources/share/acecode/models_dev"
models_dev_files=(api.json MANIFEST.json LICENSE)
for models_dev_file in "${models_dev_files[@]}"; do
    if [[ ! -f "$models_dev_dir/$models_dev_file" ]]; then
        echo "ACECode.app is missing bundled models.dev resource: $models_dev_file" >&2
        exit 1
    fi
done

plist_value() {
    /usr/libexec/PlistBuddy -c "Print :$1" "$info_plist" 2>/dev/null || true
}

bundle_identifier="$(plist_value CFBundleIdentifier)"
bundle_version="$(plist_value CFBundleShortVersionString)"
bundle_executable="$(plist_value CFBundleExecutable)"
minimum_system_version="$(plist_value LSMinimumSystemVersion)"

if [[ "$bundle_identifier" != "dev.acecode.desktop" ]]; then
    echo "Unexpected ACECode bundle identifier: ${bundle_identifier:-missing}" >&2
    exit 1
fi
if [[ ! "$bundle_version" =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ ]]; then
    echo "Unsafe or missing ACECode bundle version: ${bundle_version:-missing}" >&2
    exit 1
fi
if [[ "$bundle_executable" != "ACECode" ]]; then
    echo "Unexpected ACECode bundle executable: ${bundle_executable:-missing}" >&2
    exit 1
fi
if [[ ! "$minimum_system_version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]]; then
    echo "Unsafe or missing minimum macOS version: ${minimum_system_version:-missing}" >&2
    exit 1
fi

app_executable="$app_path/Contents/MacOS/$bundle_executable"
if [[ ! -x "$app_executable" ]]; then
    echo "Missing executable ACECode payload: $app_executable" >&2
    exit 1
fi

architectures="$(/usr/bin/lipo -archs "$app_executable" 2>/dev/null || true)"
if [[ -z "$architectures" ]]; then
    echo "Could not determine ACECode application architectures." >&2
    exit 1
fi
host_architectures=""
for architecture in $architectures; do
    case "$architecture" in
        x86_64|arm64)
            ;;
        *)
            echo "Unsupported ACECode package architecture: $architecture" >&2
            exit 1
            ;;
    esac
    if [[ -n "$host_architectures" ]]; then
        host_architectures+=","
    fi
    host_architectures+="$architecture"
done

# Keep the temporary product beside the requested output so the final move is
# an atomic rename on the same filesystem.
temporary_root="$(mktemp -d "${output_dir%/}/.acecode-pkg.XXXXXX")"
component_package="$temporary_root/dev.acecode.desktop.pkg"
distribution_path="$temporary_root/distribution.xml"
temporary_product="$temporary_root/ACECode.pkg"
expanded_component="$temporary_root/component-expanded"
expanded_product="$temporary_root/product-expanded"
domain_info="$temporary_root/domains.plist"

cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

/usr/bin/pkgbuild \
    --component "$app_path" \
    --identifier "dev.acecode.desktop.pkg" \
    --version "$bundle_version" \
    --install-location /Applications \
    --ownership recommended \
    "$component_package"

/usr/sbin/pkgutil --expand "$component_package" "$expanded_component"
package_info="$expanded_component/PackageInfo"
if [[ ! -f "$package_info" ]] ||
   ! grep -Fq 'install-location="/Applications"' "$package_info" ||
   ! grep -Fq 'relocatable="false"' "$package_info"; then
    echo "Component package is not fixed to the user-domain Applications location." >&2
    exit 1
fi
if find "$expanded_component" -type d -name Scripts -print -quit | grep -q .; then
    echo "ACECode installer package must not contain install scripts." >&2
    exit 1
fi
# PackageInfo's legacy auth attribute may still read "root" for a pkgbuild
# component. Apple deprecates that attribute for product archives: the
# Distribution installation domain below determines authorization. A
# CurrentUserHomeDirectory install runs as the current user and cannot write
# outside that user's home directory.

cat > "$distribution_path" <<DISTRIBUTION
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>ACECode</title>
    <options customize="never" require-scripts="false" hostArchitectures="$host_architectures"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="$minimum_system_version"/>
        </allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" title="ACECode" visible="false">
        <pkg-ref id="dev.acecode.desktop.pkg"/>
    </choice>
    <pkg-ref id="dev.acecode.desktop.pkg" version="$bundle_version" onConclusion="none">dev.acecode.desktop.pkg</pkg-ref>
    <pkg-ref id="dev.acecode.desktop.pkg">
        <must-close>
            <app id="dev.acecode.desktop"/>
        </must-close>
    </pkg-ref>
</installer-gui-script>
DISTRIBUTION

declare -a productbuild_args=(
    /usr/bin/productbuild
    --distribution "$distribution_path"
    --package-path "$temporary_root"
)
if [[ -n "$installer_identity" ]]; then
    productbuild_args+=(--sign "$installer_identity")
    if [[ -n "$keychain" ]]; then
        productbuild_args+=(--keychain "$keychain")
    fi
fi
productbuild_args+=("$temporary_product")
"${productbuild_args[@]}"

/usr/sbin/installer -dominfo -pkg "$temporary_product" -plist > "$domain_info"
domain_count="$(grep -Fc '<key>Domain</key>' "$domain_info")"
current_user_count="$(grep -Fc '<string>CurrentUserHomeDirectory</string>' "$domain_info")"
if [[ "$domain_count" -ne 1 || "$current_user_count" -ne 1 ]] ||
   grep -Eq '<string>(LocalSystem|NetworkDomain)</string>' "$domain_info"; then
    echo "Installer package is not restricted to CurrentUserHomeDirectory." >&2
    cat "$domain_info" >&2
    exit 1
fi

/usr/sbin/pkgutil --expand-full "$temporary_product" "$expanded_product"
expanded_distribution="$expanded_product/Distribution"
if [[ ! -f "$expanded_distribution" ]] ||
   ! grep -Fq 'enable_anywhere="false"' "$expanded_distribution" ||
   ! grep -Fq 'enable_currentUserHome="true"' "$expanded_distribution" ||
   ! grep -Fq 'enable_localSystem="false"' "$expanded_distribution" ||
   ! grep -Fq '<must-close>' "$expanded_distribution" ||
   ! grep -Fq '<app id="dev.acecode.desktop"/>' "$expanded_distribution"; then
    echo "Expanded installer package does not preserve the user-only distribution contract." >&2
    exit 1
fi
if grep -Fq '<relocate>' "$expanded_distribution"; then
    echo "Expanded ACECode product must not permit bundle relocation." >&2
    exit 1
fi
if find "$expanded_product" -type d -name Scripts -print -quit | grep -q .; then
    echo "Expanded ACECode product must not contain install scripts." >&2
    exit 1
fi
expanded_models_dev_dir="$(find "$expanded_product" -type d \
    -path '*/ACECode.app/Contents/Resources/share/acecode/models_dev' \
    -print -quit)"
if [[ -z "$expanded_models_dev_dir" ]]; then
    echo "Expanded ACECode product is missing the bundled models.dev registry." >&2
    exit 1
fi
for models_dev_file in "${models_dev_files[@]}"; do
    if [[ ! -f "$expanded_models_dev_dir/$models_dev_file" ]]; then
        echo "Expanded ACECode product is missing models.dev resource: $models_dev_file" >&2
        exit 1
    fi
done

if [[ -n "$installer_identity" ]]; then
    signature_output="$(/usr/sbin/pkgutil --check-signature "$temporary_product" 2>&1)" || {
        printf '%s\n' "$signature_output" >&2
        exit 1
    }
    printf '%s\n' "$signature_output"
    if ! grep -Fq 'Developer ID Installer:' <<< "$signature_output"; then
        echo "Installer package is not signed with Developer ID Installer." >&2
        exit 1
    fi
fi

/bin/mv "$temporary_product" "$output_path"
echo "Created and verified current-user macOS installer package: $output_path"
