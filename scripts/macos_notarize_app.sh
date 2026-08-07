#!/usr/bin/env bash
set -euo pipefail
set +x

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_notarize_app.sh --app <signed ACECode.app> \
      [--keychain-profile <name>]

Authentication:
  Local: pass --keychain-profile, or set NOTARYTOOL_PROFILE.
  CI:    set APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD.

The command archives the signed app with ditto, submits it to Apple, requires
an Accepted response, staples the ticket to the original app, validates the
ticket and code signature, and performs a Gatekeeper execution assessment.
USAGE
}

app_path=""
profile_name="${NOTARYTOOL_PROFILE:-}"

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
        --keychain-profile)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --keychain-profile" >&2
                exit 2
            fi
            profile_name="$2"
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
    echo "macOS app notarization must run on Darwin." >&2
    exit 1
fi
if [[ -z "$app_path" ]]; then
    echo "--app is required." >&2
    usage >&2
    exit 2
fi
if [[ ! -d "$app_path" || "$(basename "$app_path")" != "ACECode.app" ]]; then
    echo "Missing ACECode.app notarization input: $app_path" >&2
    exit 1
fi

declare -a authentication_args
if [[ -n "$profile_name" ]]; then
    authentication_args=(--keychain-profile "$profile_name")
else
    if [[ -z "${APPLE_ID:-}" || -z "${APPLE_TEAM_ID:-}" ||
          -z "${APPLE_APP_SPECIFIC_PASSWORD:-}" ]]; then
        echo "Provide --keychain-profile or set APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD." >&2
        exit 2
    fi
    authentication_args=(
        --apple-id "$APPLE_ID"
        --team-id "$APPLE_TEAM_ID"
        --password "$APPLE_APP_SPECIFIC_PASSWORD"
    )
fi

/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-app-notary.XXXXXX")"
archive_path="$temporary_root/ACECode.app.zip"
result_file="$temporary_root/result.json"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

# Apple's custom notarization workflow requires ditto for app ZIP submissions.
/usr/bin/ditto -c -k --keepParent "$app_path" "$archive_path"

echo "Submitting $app_path to the Apple notary service"
xcrun notarytool submit "$archive_path" \
    "${authentication_args[@]}" \
    --wait \
    --output-format json | tee "$result_file"

status="$(/usr/bin/plutil -extract status raw -o - "$result_file" 2>/dev/null || true)"
submission_id="$(/usr/bin/plutil -extract id raw -o - "$result_file" 2>/dev/null || true)"
if [[ "$status" != "Accepted" ]]; then
    echo "Apple app notarization was not accepted (status: ${status:-unknown})." >&2
    if [[ -n "$submission_id" ]]; then
        xcrun notarytool log "$submission_id" "${authentication_args[@]}" || true
    fi
    exit 1
fi

echo "Stapling notarization ticket to $app_path"
xcrun stapler staple "$app_path"
xcrun stapler validate "$app_path"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"

echo "Assessing $app_path with Gatekeeper"
/usr/sbin/spctl --assess --type execute --verbose=4 "$app_path"

echo "Notarized, stapled, and validated $app_path"
