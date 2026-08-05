#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_create_dmg.sh --app <ACECode.app> \
      --installer <Install ACECode.app> --output <ACECode.dmg> \
      [--volume-name <name>] [--instructions <path>]

Creates a compressed read-only DMG containing ACECode.app, the current-user
installer, and bilingual instructions. The image never contains a link to the
system /Applications directory.
USAGE
}

app_path=""
installer_path=""
output_path=""
volume_name="ACECode"
instructions_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --app" >&2; exit 2; }
            app_path="$2"
            shift 2
            ;;
        --installer)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --installer" >&2; exit 2; }
            installer_path="$2"
            shift 2
            ;;
        --output)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --output" >&2; exit 2; }
            output_path="$2"
            shift 2
            ;;
        --volume-name)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --volume-name" >&2; exit 2; }
            volume_name="$2"
            shift 2
            ;;
        --instructions)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --instructions" >&2; exit 2; }
            instructions_path="$2"
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
    echo "DMG creation must run on macOS." >&2
    exit 1
fi

if [[ -z "$app_path" || -z "$installer_path" || -z "$output_path" ]]; then
    echo "--app, --installer, and --output are required." >&2
    usage >&2
    exit 2
fi

if [[ "$output_path" != *.dmg ]]; then
    echo "DMG output must end in .dmg: $output_path" >&2
    exit 2
fi

if [[ ! -d "$app_path" ]]; then
    echo "Missing ACECode app bundle: $app_path" >&2
    exit 1
fi
if [[ ! -d "$installer_path" ]]; then
    echo "Missing installer app bundle: $installer_path" >&2
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "$instructions_path" ]]; then
    instructions_path="$script_dir/../assets/macos/DMG_INSTALL.txt"
fi
if [[ ! -f "$instructions_path" ]]; then
    echo "Missing DMG instructions: $instructions_path" >&2
    exit 1
fi

output_parent="$(dirname "$output_path")"
mkdir -p "$output_parent"

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-dmg.XXXXXX")"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

staging_root="$temporary_root/root"
mkdir -p "$staging_root"

/usr/bin/ditto "$app_path" "$staging_root/ACECode.app"
/usr/bin/ditto "$installer_path" "$staging_root/Install ACECode.app"
/usr/bin/ditto "$instructions_path" "$staging_root/Install Instructions 安装说明.txt"

if [[ -e "$staging_root/Applications" || -L "$staging_root/Applications" ]]; then
    echo "Refusing to package a system /Applications link." >&2
    exit 1
fi

/usr/bin/hdiutil create \
    -volname "$volume_name" \
    -srcfolder "$staging_root" \
    -format UDZO \
    -ov \
    "$output_path"

/usr/bin/hdiutil verify "$output_path"
echo "Created $output_path"
