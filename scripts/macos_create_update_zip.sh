#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_create_update_zip.sh --app <ACECode.app> \
      --output <ACECode-version-macos-arch-update.zip> [--require-trusted]

Creates a self-update ZIP with Apple's ditto tool. The archive contains the
complete app plus a root `acecode` copied from its notarized bundled daemon and
a root `share/acecode/models_dev` registry so standalone CLI installs retain
their existing flat upgrade path and offline model catalog. The command extracts
into a clean temporary directory and verifies executable permissions/resources.
--require-trusted also requires strict signatures, a stapled app ticket, and
Gatekeeper acceptance; tagged release jobs use this mode after app notarization.
USAGE
}

app_path=""
output_path=""
require_trusted=false

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
        --require-trusted)
            require_trusted=true
            shift
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
    echo "macOS update ZIP creation must run on Darwin." >&2
    exit 1
fi
if [[ -z "$app_path" || -z "$output_path" ]]; then
    echo "--app and --output are required." >&2
    usage >&2
    exit 2
fi
if [[ ! -d "$app_path" || "$(basename "$app_path")" != "ACECode.app" ]]; then
    echo "Missing ACECode.app update payload: $app_path" >&2
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
if [[ "$output_path" != *.zip ]]; then
    echo "Update archive output must end in .zip: $output_path" >&2
    exit 2
fi
output_dir="$(dirname "$output_path")"
if [[ ! -d "$output_dir" ]]; then
    echo "Update archive output directory does not exist: $output_dir" >&2
    exit 1
fi

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-update-zip.XXXXXX")"
temporary_zip="$temporary_root/update.zip"
verify_root="$temporary_root/extracted"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

/usr/bin/ditto -c -k --keepParent "$app_path" "$temporary_zip"
# The manifest target is shared by desktop and standalone CLI installations.
# Reuse the already signed/notarized bundled daemon as the flat CLI payload.
/usr/bin/ditto "$app_path/Contents/MacOS/acecode-daemon" \
    "$temporary_root/acecode"
mkdir -p "$temporary_root/share/acecode"
/usr/bin/ditto "$models_dev_dir" \
    "$temporary_root/share/acecode/models_dev"
(
    cd "$temporary_root"
    /usr/bin/zip -qr "$temporary_zip" acecode share
)
mkdir -p "$verify_root"
/usr/bin/ditto -x -k "$temporary_zip" "$verify_root"

verified_app="$verify_root/ACECode.app"
app_main="$verified_app/Contents/MacOS/ACECode"
app_daemon="$verified_app/Contents/MacOS/acecode-daemon"
verified_cli="$verify_root/acecode"
verified_app_models="$verified_app/Contents/Resources/share/acecode/models_dev"
verified_cli_models="$verify_root/share/acecode/models_dev"
if [[ ! -d "$verified_app" || ! -x "$app_main" || ! -x "$app_daemon" ||
      ! -x "$verified_cli" ]]; then
    echo "Extracted update archive is missing executable ACECode app or CLI binaries." >&2
    exit 1
fi
for models_dev_file in "${models_dev_files[@]}"; do
    if [[ ! -f "$verified_app_models/$models_dev_file" ||
          ! -f "$verified_cli_models/$models_dev_file" ]] ||
       ! /usr/bin/cmp -s "$verified_app_models/$models_dev_file" \
           "$verified_cli_models/$models_dev_file"; then
        echo "Extracted update archive has missing or mismatched models.dev resource: $models_dev_file" >&2
        exit 1
    fi
done

if [[ "$require_trusted" == true ]]; then
    /usr/bin/codesign --verify --deep --strict --verbose=2 "$verified_app"
    /usr/bin/codesign --verify --strict --verbose=2 "$verified_cli"
    xcrun stapler validate "$verified_app"
    /usr/sbin/spctl --assess --type execute --verbose=4 "$verified_app"
fi

/bin/mv -f "$temporary_zip" "$output_path"
echo "Created and verified macOS update archive: $output_path"
