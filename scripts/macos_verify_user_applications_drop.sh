#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_verify_user_applications_drop.sh \
      --bundle <Applications.app>

Verifies on macOS that Launch Services delivers a dragged application bundle
to the current-user Applications droplet and that the droplet installs it into
~/Applications without requiring a second click.
USAGE
}

bundle_path=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle)
            [[ $# -ge 2 && -n "${2:-}" ]] || {
                echo "Missing value for --bundle" >&2
                exit 2
            }
            bundle_path="$2"
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
    echo "Applications drop verification must run on macOS." >&2
    exit 1
fi
if [[ -z "$bundle_path" ]]; then
    echo "--bundle is required." >&2
    usage >&2
    exit 2
fi
if [[ ! -d "$bundle_path" ]]; then
    echo "Missing Applications bundle: $bundle_path" >&2
    exit 1
fi

bundle_path="$(cd "$(dirname "$bundle_path")" && pwd -P)/$(basename "$bundle_path")"
info_plist="$bundle_path/Contents/Info.plist"
if [[ ! -f "$info_plist" ]]; then
    echo "Missing Applications Info.plist: $info_plist" >&2
    exit 1
fi

drop_uti="$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleDocumentTypes:0:LSItemContentTypes:0' \
    "$info_plist")"
handler_rank="$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleDocumentTypes:0:LSHandlerRank' \
    "$info_plist")"
if [[ "$drop_uti" != "public.item" ]]; then
    echo "Applications droplet must accept public.item, got: $drop_uti" >&2
    exit 1
fi
if [[ "$handler_rank" != "None" ]]; then
    echo "Applications droplet must use LSHandlerRank=None, got: $handler_rank" >&2
    exit 1
fi

if [[ -z "${HOME:-}" || "$HOME" != /* || ! -d "$HOME" ]]; then
    echo "A real absolute HOME is required for drop verification." >&2
    exit 1
fi
user_home="$(cd "$HOME" && pwd -P)"
applications_dir="$user_home/Applications"
destination="$applications_dir/ACECode.app"
case "$destination" in
    "$user_home/Applications/ACECode.app") ;;
    *)
        echo "Unsafe drop verification destination: $destination" >&2
        exit 1
        ;;
esac
if [[ -e "$destination" || -L "$destination" ]]; then
    echo "Drop verification refuses to replace an existing app: $destination" >&2
    exit 1
fi
applications_preexisted=0
if [[ -d "$applications_dir" ]]; then
    applications_preexisted=1
fi

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-drop-target.XXXXXX")"
drop_root="$temporary_root/ACECode Drop Test"
marker_relative="Contents/Resources/acecode-drop-smoke-marker"
helper_exec="$drop_root/Applications.app/Contents/MacOS/Applications"

cleanup() {
    if [[ -n "${open_pid:-}" ]] && /bin/kill -0 "$open_pid" 2>/dev/null; then
        /bin/kill "$open_pid" >/dev/null 2>&1 || true
    fi
    if [[ -n "${helper_exec:-}" ]]; then
        while IFS= read -r pid; do
            [[ -n "$pid" ]] && /bin/kill "$pid" >/dev/null 2>&1 || true
        done < <(/usr/bin/pgrep -f -x "$helper_exec" 2>/dev/null || true)
    fi
    if [[ -n "${destination:-}" &&
          -f "$destination/${marker_relative:-missing}" ]]; then
        rm -rf -- "$destination"
    fi
    if [[ "${applications_preexisted:-1}" -eq 0 &&
          -d "${applications_dir:-}" ]]; then
        /bin/rmdir "$applications_dir" >/dev/null 2>&1 || true
    fi
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

mkdir -p "$drop_root/ACECode.app/Contents/MacOS" \
         "$drop_root/ACECode.app/Contents/Resources"
/usr/bin/ditto "$bundle_path" "$drop_root/Applications.app"
/bin/cp /usr/bin/true "$drop_root/ACECode.app/Contents/MacOS/ACECode"
/usr/bin/touch "$drop_root/ACECode.app/$marker_relative"
cat >"$drop_root/ACECode.app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>ACECode</string>
  <key>CFBundleIdentifier</key>
  <string>dev.acecode.desktop</string>
  <key>CFBundleName</key>
  <string>ACECode Drop Test</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.0.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
</dict>
</plist>
PLIST

/usr/bin/open -W -n -a "$drop_root/Applications.app" \
    "$drop_root/ACECode.app" &
open_pid=$!

installed=0
for _ in {1..200}; do
    if [[ -f "$destination/$marker_relative" ]]; then
        installed=1
        break
    fi
    sleep 0.1
done
if [[ "$installed" -ne 1 ]]; then
    echo "Finder/Launch Services drop did not install ACECode." >&2
    exit 1
fi

helper_finished=0
for _ in {1..100}; do
    if ! /bin/kill -0 "$open_pid" 2>/dev/null; then
        helper_finished=1
        break
    fi
    sleep 0.1
done
if [[ "$helper_finished" -ne 1 ]]; then
    echo "Applications droplet did not finish without an extra click." >&2
    exit 1
fi
wait "$open_pid"
open_pid=""

if [[ ! -x "$destination/Contents/MacOS/ACECode" ]]; then
    echo "Dropped ACECode executable was not installed." >&2
    exit 1
fi
installed_identifier="$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleIdentifier' "$destination/Contents/Info.plist")"
if [[ "$installed_identifier" != "dev.acecode.desktop" ]]; then
    echo "Dropped ACECode bundle identifier changed during installation." >&2
    exit 1
fi

echo "macOS Applications droplet integration check passed"
