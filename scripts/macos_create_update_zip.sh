#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_create_update_zip.sh --app <ACECode.app> \
      --output <ACECode-version-macos-arch-update.zip> [--require-trusted]

Creates a self-update ZIP with Apple's ditto tool. The archive contains the
complete app plus a small root `acecode` copied from its notarized bundled CLI
forwarder. The daemon, helper and resources exist only inside the app; existing
flat CLI updaters still find a root executable. The command extracts into a clean
temporary directory and verifies executable permissions and bundled resources.
--require-trusted also requires strict signatures, a stapled app ticket, and
Gatekeeper acceptance; tagged release jobs use this mode after app notarization.
USAGE
}

app_path=""
output_path=""
require_trusted=false
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/.." && pwd -P)"

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
computer_use_helper="$app_path/Contents/MacOS/acecode-computer-use"
cli_launcher="$app_path/Contents/MacOS/acecode-cli"
if [[ ! -x "$cli_launcher" || ! -s "$cli_launcher" ]]; then
    echo "ACECode.app is missing the CLI forwarder: $cli_launcher" >&2
    exit 1
fi
# A copied full daemon would silently restore the old duplicate payload.
if [[ "$(wc -c < "$cli_launcher" | tr -d '[:space:]')" -gt 262144 ]]; then
    echo "CLI forwarder exceeds the 256 KiB package budget" >&2
    exit 1
fi
if [[ ! -x "$computer_use_helper" || ! -s "$computer_use_helper" ]]; then
    echo "ACECode.app is missing the Computer Use helper: $computer_use_helper" >&2
    exit 1
fi
models_dev_dir="$app_path/Contents/Resources/share/acecode/models_dev"
seed_dir="$app_path/Contents/Resources/share/acecode/seed"
models_dev_files=(api.json MANIFEST.json LICENSE)
for models_dev_file in "${models_dev_files[@]}"; do
    if [[ ! -f "$models_dev_dir/$models_dev_file" ]]; then
        echo "ACECode.app is missing bundled models.dev resource: $models_dev_file" >&2
        exit 1
    fi
done
python3 "$repo_root/scripts/verify_seed_bundle.py" \
    --source "$repo_root/assets/seed" \
    --packaged "$seed_dir"
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
# Reuse only the signed/notarized tiny launcher. All runtime data stays in the app.
/usr/bin/ditto "$cli_launcher" "$temporary_root/acecode"
(
    cd "$temporary_root"
    /usr/bin/zip -q "$temporary_zip" acecode
)
mkdir -p "$verify_root"
/usr/bin/ditto -x -k "$temporary_zip" "$verify_root"

verified_app="$verify_root/ACECode.app"
app_main="$verified_app/Contents/MacOS/ACECode"
app_daemon="$verified_app/Contents/MacOS/acecode-daemon"
verified_cli="$verify_root/acecode"
verified_app_models="$verified_app/Contents/Resources/share/acecode/models_dev"
verified_app_seed="$verified_app/Contents/Resources/share/acecode/seed"
if [[ ! -d "$verified_app" || ! -x "$app_main" || ! -x "$app_daemon" ||
      ! -x "$verified_cli" || ! -x "$verified_app/Contents/MacOS/acecode-cli" ||
      ! -x "$verified_app/Contents/MacOS/acecode-computer-use" ]]; then
    echo "Extracted update archive is missing executable ACECode app or CLI binaries." >&2
    exit 1
fi
if ! /usr/bin/cmp -s "$verified_cli" "$verified_app/Contents/MacOS/acecode-cli"; then
    echo "Extracted CLI forwarder differs from the signed app copy." >&2
    exit 1
fi
for duplicate in share channels acecode-computer-use; do
    if [[ -e "$verify_root/$duplicate" ]]; then
        echo "Duplicated macOS runtime resource: $duplicate" >&2
        exit 1
    fi
done
for models_dev_file in "${models_dev_files[@]}"; do
    if [[ ! -f "$verified_app_models/$models_dev_file" ]] ||
       ! /usr/bin/cmp -s "$verified_app_models/$models_dev_file" "$models_dev_dir/$models_dev_file"; then
        echo "Extracted update archive has missing or mismatched models.dev resource: $models_dev_file" >&2
        exit 1
    fi
done
python3 "$repo_root/scripts/verify_seed_bundle.py" \
    --source "$repo_root/assets/seed" \
    --packaged "$verified_app_seed"

if [[ "$require_trusted" == true ]]; then
    /usr/bin/codesign --verify --deep --strict --verbose=2 "$verified_app"
    /usr/bin/codesign --verify --strict --verbose=2 "$verified_cli"
    /usr/bin/codesign --verify --strict --verbose=2 "$verified_app/Contents/MacOS/acecode-computer-use"
    xcrun stapler validate "$verified_app"
    /usr/sbin/spctl --assess --type execute --verbose=4 "$verified_app"
fi

/bin/mv -f "$temporary_zip" "$output_path"
echo "Created and verified macOS update archive: $output_path"
