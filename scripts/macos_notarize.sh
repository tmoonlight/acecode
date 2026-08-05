#!/usr/bin/env bash
set -euo pipefail
set +x

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_notarize.sh --file <signed.dmg> [--keychain-profile <name>]

Authentication:
  Local: pass --keychain-profile, or set NOTARYTOOL_PROFILE.
  CI:    set APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD.

The command waits for Apple, requires an Accepted response, staples and validates
the ticket, then performs a Gatekeeper assessment of the DMG.
USAGE
}

file_path=""
profile_name="${NOTARYTOOL_PROFILE:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --file)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --file" >&2
                exit 2
            fi
            file_path="$2"
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
    echo "macOS notarization must run on Darwin." >&2
    exit 1
fi
if [[ -z "$file_path" ]]; then
    echo "--file is required." >&2
    usage >&2
    exit 2
fi
if [[ ! -f "$file_path" ]]; then
    echo "Missing notarization input: $file_path" >&2
    exit 1
fi
if [[ "$file_path" != *.dmg ]]; then
    echo "This release helper accepts a signed DMG: $file_path" >&2
    exit 2
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

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
result_file="$(mktemp "${temporary_base}/acecode-notary.XXXXXX.json")"
cleanup() {
    if [[ -n "${result_file:-}" && -f "$result_file" ]]; then
        rm -f -- "$result_file"
    fi
}
trap cleanup EXIT

echo "Submitting $file_path to the Apple notary service"
xcrun notarytool submit "$file_path" \
    "${authentication_args[@]}" \
    --wait \
    --output-format json | tee "$result_file"

status="$(/usr/bin/plutil -extract status raw -o - "$result_file" 2>/dev/null || true)"
submission_id="$(/usr/bin/plutil -extract id raw -o - "$result_file" 2>/dev/null || true)"
if [[ "$status" != "Accepted" ]]; then
    echo "Apple notarization was not accepted (status: ${status:-unknown})." >&2
    if [[ -n "$submission_id" ]]; then
        xcrun notarytool log "$submission_id" "${authentication_args[@]}" || true
    fi
    exit 1
fi

echo "Stapling notarization ticket to $file_path"
xcrun stapler staple "$file_path"
xcrun stapler validate "$file_path"
/usr/bin/codesign --verify --strict --verbose=2 "$file_path"

echo "Assessing $file_path with Gatekeeper"
/usr/sbin/spctl --assess \
    --type open \
    --context context:primary-signature \
    --verbose=4 \
    "$file_path"

echo "Notarized and validated $file_path"
