#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_codesign.sh --identity <codesign identity> [--keychain <path>] \
      [--binary <path> ...] [--app <ACECode.app>]

Signs macOS command-line binaries and app bundles with hardened runtime enabled.
Nested app executables are signed before the app bundle itself.
USAGE
}

identity=""
keychain=""
app_path=""
declare -a binaries=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --identity)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --identity" >&2
                exit 2
            fi
            identity="$2"
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
        --binary)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --binary" >&2
                exit 2
            fi
            binaries+=("$2")
            shift 2
            ;;
        --app)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --app" >&2
                exit 2
            fi
            app_path="$2"
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
    echo "macOS signing must run on Darwin; found $(uname -s)" >&2
    exit 1
fi

if [[ -z "$identity" ]]; then
    echo "A codesign identity is required." >&2
    usage >&2
    exit 2
fi

if [[ ${#binaries[@]} -eq 0 && -z "$app_path" ]]; then
    echo "Nothing to sign. Pass at least one --binary or --app." >&2
    usage >&2
    exit 2
fi

declare -a codesign_args=(
    /usr/bin/codesign
    --force
    --timestamp
    --options
    runtime
    --sign
    "$identity"
)

if [[ -n "$keychain" ]]; then
    codesign_args+=(--keychain "$keychain")
fi

sign_path() {
    local path="$1"

    if [[ ! -e "$path" ]]; then
        echo "Missing path to sign: $path" >&2
        exit 1
    fi

    echo "Signing $path"
    "${codesign_args[@]}" "$path"
}

verify_binary() {
    local path="$1"

    echo "Verifying $path"
    /usr/bin/codesign --verify --strict --verbose=2 "$path"
}

verify_app() {
    local path="$1"

    echo "Verifying $path"
    /usr/bin/codesign --verify --deep --strict --verbose=2 "$path"
}

sign_app_executable() {
    local path="$1"

    if [[ -f "$path" ]]; then
        sign_path "$path"
    else
        echo "Missing app executable: $path" >&2
        exit 1
    fi
}

sign_extra_app_code() {
    local root="$1"
    local skip_path="${2:-}"
    local skip_path_two="${3:-}"

    if [[ ! -d "$root" ]]; then
        return
    fi

    while IFS= read -r -d '' code_path; do
        if [[ "$code_path" == "$skip_path" || "$code_path" == "$skip_path_two" ]]; then
            continue
        fi
        sign_path "$code_path"
    done < <(
        find "$root" \
            \( -type f \( -name '*.dylib' -o -name '*.so' -o -perm -111 \) \) \
            -print0
    )
}

for binary in "${binaries[@]}"; do
    sign_path "$binary"
    verify_binary "$binary"
done

if [[ -n "$app_path" ]]; then
    if [[ ! -d "$app_path" ]]; then
        echo "Missing app bundle: $app_path" >&2
        exit 1
    fi

    app_main="$app_path/Contents/MacOS/ACECode"
    app_daemon="$app_path/Contents/MacOS/acecode-daemon"
    sign_app_executable "$app_main"
    sign_app_executable "$app_daemon"
    sign_extra_app_code "$app_path/Contents/MacOS" "$app_main" "$app_daemon"
    sign_extra_app_code "$app_path/Contents/Frameworks"
    sign_path "$app_path"
    verify_app "$app_path"
fi
