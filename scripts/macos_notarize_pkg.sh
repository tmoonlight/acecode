#!/usr/bin/env bash
set -euo pipefail
set +x

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_notarize_pkg.sh --pkg <signed.pkg> \
      [--keychain-profile <name>]

Authentication:
  Local: pass --keychain-profile, or set NOTARYTOOL_PROFILE.
  CI:    set APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD.

The command requires a signed Installer package, waits for Apple notarization,
staples and validates the ticket, rechecks the package signature, and performs
a Gatekeeper install assessment.
USAGE
}

pkg_path=""
profile_name="${NOTARYTOOL_PROFILE:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pkg)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Missing value for --pkg" >&2
                exit 2
            fi
            pkg_path="$2"
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
    echo "macOS PKG notarization must run on Darwin." >&2
    exit 1
fi
if [[ -z "$pkg_path" ]]; then
    echo "--pkg is required." >&2
    usage >&2
    exit 2
fi
if [[ ! -f "$pkg_path" ]]; then
    echo "Missing Installer package: $pkg_path" >&2
    exit 1
fi
if [[ "$pkg_path" != *.pkg ]]; then
    echo "PKG notarization input must end in .pkg: $pkg_path" >&2
    exit 2
fi

echo "Verifying Installer package signature"
signature_output="$(/usr/sbin/pkgutil --check-signature "$pkg_path" 2>&1)" || {
    printf '%s\n' "$signature_output" >&2
    exit 1
}
printf '%s\n' "$signature_output"
if ! grep -Fq 'Developer ID Installer:' <<< "$signature_output"; then
    echo "Installer package is not signed with Developer ID Installer." >&2
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

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
result_file="$(mktemp "${temporary_base}/acecode-pkg-notary.XXXXXX.json")"
cleanup() {
    if [[ -n "${result_file:-}" && -f "$result_file" ]]; then
        rm -f -- "$result_file"
    fi
}
trap cleanup EXIT

echo "Submitting $pkg_path to the Apple notary service"
xcrun notarytool submit "$pkg_path" \
    "${authentication_args[@]}" \
    --wait \
    --output-format json | tee "$result_file"

notary_status="$(/usr/bin/plutil -extract status raw -o - "$result_file" 2>/dev/null || true)"
submission_id="$(/usr/bin/plutil -extract id raw -o - "$result_file" 2>/dev/null || true)"
if [[ "$notary_status" != "Accepted" ]]; then
    echo "Apple notarization was not accepted (status: ${notary_status:-unknown})." >&2
    if [[ -n "$submission_id" ]]; then
        xcrun notarytool log "$submission_id" "${authentication_args[@]}" || true
    fi
    exit 1
fi

echo "Stapling notarization ticket to $pkg_path"
xcrun stapler staple "$pkg_path"
xcrun stapler validate "$pkg_path"
post_staple_signature="$(/usr/sbin/pkgutil --check-signature "$pkg_path" 2>&1)" || {
    printf '%s\n' "$post_staple_signature" >&2
    exit 1
}
printf '%s\n' "$post_staple_signature"
if ! grep -Fq 'Developer ID Installer:' <<< "$post_staple_signature"; then
    echo "Stapled package lost its Developer ID Installer signature." >&2
    exit 1
fi

echo "Assessing $pkg_path with Gatekeeper"
/usr/sbin/spctl --assess \
    --type install \
    --verbose=4 \
    "$pkg_path"

echo "Notarized and validated $pkg_path"
