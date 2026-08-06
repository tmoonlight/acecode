#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_generate_icns.sh --source <icon.svg> --output <icon.icns>

Renders an SVG at every macOS iconset size using sips, then creates an ICNS
file using iconutil. This helper must run on macOS.
USAGE
}

source_path=""
output_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --source" >&2; exit 2; }
            source_path="$2"
            shift 2
            ;;
        --output)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --output" >&2; exit 2; }
            output_path="$2"
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

if [[ -z "$source_path" || -z "$output_path" ]]; then
    echo "--source and --output are required." >&2
    usage >&2
    exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "ICNS generation must run on macOS." >&2
    exit 1
fi
if [[ ! -f "$source_path" ]]; then
    echo "Missing icon source: $source_path" >&2
    exit 1
fi
if [[ "$output_path" != *.icns ]]; then
    echo "ICNS output must end in .icns: $output_path" >&2
    exit 2
fi

output_parent="$(dirname "$output_path")"
mkdir -p "$output_parent"

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-icon.XXXXXX")"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

iconset_path="$temporary_root/acecode-installer.iconset"
mkdir -p "$iconset_path"

render_icon() {
    local pixels="$1"
    local filename="$2"
    /usr/bin/sips -s format png -z "$pixels" "$pixels" \
        "$source_path" --out "$iconset_path/$filename" >/dev/null
}

render_icon 16 icon_16x16.png
render_icon 32 icon_16x16@2x.png
render_icon 32 icon_32x32.png
render_icon 64 icon_32x32@2x.png
render_icon 128 icon_128x128.png
render_icon 256 icon_128x128@2x.png
render_icon 256 icon_256x256.png
render_icon 512 icon_256x256@2x.png
render_icon 512 icon_512x512.png
render_icon 1024 icon_512x512@2x.png

/usr/bin/iconutil -c icns "$iconset_path" -o "$output_path"
echo "Created $output_path"
