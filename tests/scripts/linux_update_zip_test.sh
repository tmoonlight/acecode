#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
create_script="$repo_root/scripts/linux_create_update_zip.sh"
temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-linux-update-test.XXXXXX")"
trap 'rm -rf -- "$temporary_root"' EXIT

for target in linux-x64 linux-arm64; do
    package_dir="$temporary_root/acecode-$target"
    mkdir -p "$package_dir/share/acecode/models_dev"
    cp "$repo_root/assets/models_dev/api.json" \
       "$repo_root/assets/models_dev/MANIFEST.json" \
       "$repo_root/assets/models_dev/LICENSE" \
       "$package_dir/share/acecode/models_dev/"
    printf '%s\n' \
        '#!/usr/bin/env bash' \
        'printf "acecode v9.8.7\\n"' > "$package_dir/acecode"
    printf '%s\n' \
        '#!/usr/bin/env bash' \
        'exit 0' > "$package_dir/acecode-desktop"
    chmod 0755 "$package_dir/acecode" "$package_dir/acecode-desktop"

    output_zip="$temporary_root/acecode-9.8.7-$target-update.zip"
    bash "$create_script" \
        --package-dir "$package_dir" \
        --output "$output_zip" \
        --expected-version 9.8.7
    test -f "$output_zip"

    extracted_root="$temporary_root/extracted-$target"
    unzip -q "$output_zip" -d "$extracted_root"
    test -x "$extracted_root/acecode-$target/acecode"
    test -x "$extracted_root/acecode-$target/acecode-desktop"
done

chmod 0644 "$temporary_root/acecode-linux-arm64/acecode"
if bash "$create_script" \
    --package-dir "$temporary_root/acecode-linux-arm64" \
    --output "$temporary_root/must-not-exist.zip" \
    --expected-version 9.8.7; then
    echo "Non-executable Linux package unexpectedly passed validation" >&2
    exit 1
fi
test ! -e "$temporary_root/must-not-exist.zip"

printf 'Linux update ZIP contract checks passed.\n'
