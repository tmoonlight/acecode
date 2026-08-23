#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: scripts/linux_create_update_zip.sh \
  --package-dir <dist/acecode-linux-x64|dist/acecode-linux-arm64> \
  --output <acecode-version-linux-arch-update.zip> \
  --expected-version <version>
EOF
}

package_dir=""
output_path=""
expected_version=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --package-dir)
            package_dir="${2:-}"
            shift 2
            ;;
        --output)
            output_path="${2:-}"
            shift 2
            ;;
        --expected-version)
            expected_version="${2:-}"
            shift 2
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$package_dir" || -z "$output_path" || -z "$expected_version" ]]; then
    usage
    exit 2
fi
if [[ ! -d "$package_dir" ]]; then
    echo "Linux package directory does not exist: $package_dir" >&2
    exit 1
fi
for command_name in cmp find realpath unzip zip; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Required command is unavailable: $command_name" >&2
        exit 1
    fi
done

package_dir="$(realpath "$package_dir")"
package_name="$(basename "$package_dir")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(realpath "$script_dir/..")"
case "$package_name" in
    acecode-linux-x64|acecode-linux-arm64) ;;
    *)
        echo "Unsupported Linux self-update package directory: $package_name" >&2
        exit 1
        ;;
esac
for executable_name in acecode acecode-desktop; do
    if [[ ! -f "$package_dir/$executable_name" ]]; then
        echo "Linux self-update package is missing $executable_name: $package_dir" >&2
        exit 1
    fi
    if [[ ! -x "$package_dir/$executable_name" ]]; then
        echo "Linux self-update executable is not executable: $package_dir/$executable_name" >&2
        exit 1
    fi
done

unexpected_entry="$(
    find "$package_dir" -mindepth 1 \
        \( -type l -o \( ! -type f ! -type d \) \) -print -quit
)"
if [[ -n "$unexpected_entry" ]]; then
    echo "Linux self-update package contains an unsupported filesystem entry: $unexpected_entry" >&2
    exit 1
fi
legacy_artifact="$(find "$package_dir" -maxdepth 1 -iname 'ace-browser-*' -print -quit)"
if [[ -n "$legacy_artifact" ]]; then
    echo "Legacy browser artifact must not be packaged: $legacy_artifact" >&2
    exit 1
fi

validate_models_dev_registry() {
    local registry_dir="$1"
    if [[ ! -d "$registry_dir" ]]; then
        echo "Missing models.dev directory: $registry_dir" >&2
        return 1
    fi
    local file_count
    file_count="$(find "$registry_dir" -maxdepth 1 -type f | wc -l | tr -d '[:space:]')"
    if [[ "$file_count" != "3" ]]; then
        echo "Expected exactly 3 models.dev files in $registry_dir; found $file_count" >&2
        return 1
    fi
    local file_name
    for file_name in api.json MANIFEST.json LICENSE; do
        if [[ ! -f "$registry_dir/$file_name" ]] ||
           ! cmp -s "$repo_root/assets/models_dev/$file_name" \
                  "$registry_dir/$file_name"; then
            echo "Missing or mismatched models.dev package file: $registry_dir/$file_name" >&2
            return 1
        fi
    done
}

validate_version() {
    local executable_path="$1"
    local version_output
    version_output="$("$executable_path" --version)"
    if [[ "$version_output" != "acecode v$expected_version" ]]; then
        echo "Unexpected ACECode version from $executable_path: $version_output" >&2
        return 1
    fi
}

validate_models_dev_registry "$package_dir/share/acecode/models_dev"
validate_version "$package_dir/acecode"

mkdir -p "$(dirname "$output_path")"
output_path="$(realpath -m "$output_path")"
case "$output_path" in
    "$package_dir"|"$package_dir"/*)
        echo "Output ZIP must not be inside the packaged directory: $output_path" >&2
        exit 1
        ;;
esac

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-linux-update.XXXXXX")"
trap 'rm -rf -- "$temporary_root"' EXIT
temporary_zip="$temporary_root/update.zip"

(
    cd "$(dirname "$package_dir")"
    zip -q -X -r "$temporary_zip" "$package_name"
)

verify_root="$temporary_root/extracted"
mkdir -p "$verify_root"
unzip -q "$temporary_zip" -d "$verify_root"
top_level_count="$(find "$verify_root" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')"
if [[ "$top_level_count" != "1" || ! -d "$verify_root/$package_name" ]]; then
    echo "Linux self-update ZIP must contain only the $package_name top-level directory" >&2
    exit 1
fi
extracted_package="$verify_root/$package_name"
for executable_name in acecode acecode-desktop; do
    if [[ ! -x "$extracted_package/$executable_name" ]]; then
        echo "Linux self-update ZIP lost executable permissions: $executable_name" >&2
        exit 1
    fi
done
validate_models_dev_registry "$extracted_package/share/acecode/models_dev"
validate_version "$extracted_package/acecode"
legacy_artifact="$(find "$extracted_package" -maxdepth 1 -iname 'ace-browser-*' -print -quit)"
if [[ -n "$legacy_artifact" ]]; then
    echo "Legacy browser artifact found after ZIP extraction: $legacy_artifact" >&2
    exit 1
fi

mv -f -- "$temporary_zip" "$output_path"
printf 'Linux self-update ZIP ready: %s\n' "$output_path"
