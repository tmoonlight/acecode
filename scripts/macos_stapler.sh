#!/usr/bin/env bash
# Sourced by the macOS packaging scripts; it only defines acecode_stapler.
#
# `xcrun stapler` exits 68 when it cannot reach Apple's ticket service: CloudKit
# drops the connection (NSURLErrorDomain -1005 against api.apple-cloudkit.com) or
# the service asks the client to back off. The notarization has already been
# accepted at that point, so the failure is transient. A release job calls
# stapler about a dozen times; v1.0.7's macos-x64 job failed three runs in a row,
# each time on a different call, and v1.0.6 hit the same error. Retry only 68;
# a missing ticket, an invalid signature or any other failure still stops the
# build at once. Stapling and validating are idempotent, so a retry is safe.
#
# ACECODE_STAPLER_ATTEMPTS (default 6) and ACECODE_STAPLER_RETRY_DELAY (seconds,
# default 15) exist for the offline contract tests.

acecode_stapler() {
    local attempts="${ACECODE_STAPLER_ATTEMPTS:-6}"
    local delay="${ACECODE_STAPLER_RETRY_DELAY:-15}"
    local attempt=1
    local result=0
    while true; do
        if xcrun stapler "$@"; then
            return 0
        else
            result=$?
        fi
        if [[ "$result" -ne 68 || "$attempt" -ge "$attempts" ]]; then
            return "$result"
        fi
        echo "xcrun stapler $1: Apple ticket service unavailable (68), retry $attempt/$((attempts - 1)) in ${delay}s" >&2
        sleep "$delay"
        attempt=$((attempt + 1))
    done
}
