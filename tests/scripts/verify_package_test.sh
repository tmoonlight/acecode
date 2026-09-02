#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
verify_script="$repo_root/.acecode/skills/verify-package/scripts/verify_package.py"

if command -v python3 >/dev/null 2>&1 && python3 -c "" >/dev/null 2>&1; then
    python_bin="python3"
else
    python_bin="python"
fi

# The flow cases exec sh-shebang stub executables through the python script
# and resolve an extension-less sh cmake shim on PATH. Native Windows Python
# cannot exec those files and shutil.which("cmake") finds the real cmake.exe
# over the shim, so non-POSIX hosts only exercise the preflight-only cases.
# CI registers this test under if(UNIX), where every case runs.
full_mode=1
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) full_mode=0 ;;
esac

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/acecode-verify-package.XXXXXX")"
cleanup() {
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

expect_status() {
    local expected="$1"
    local label="$2"
    shift 2

    local output="$temporary_root/output.txt"
    local actual=0
    "$@" >"$output" 2>&1 || actual=$?
    if [[ "$actual" -ne "$expected" ]]; then
        echo "$label: expected exit $expected, got $actual" >&2
        sed -n '1,120p' "$output" >&2
        exit 1
    fi
}

expect_output() {
    local pattern="$1"
    local label="$2"
    if ! grep -q "$pattern" "$temporary_root/output.txt"; then
        echo "$label: output missing pattern '$pattern'" >&2
        sed -n '1,120p' "$temporary_root/output.txt" >&2
        exit 1
    fi
}

"$python_bin" -m py_compile "$verify_script"

expect_status 0 "help" "$python_bin" "$verify_script" --help
expect_status 2 "unknown argument" "$python_bin" "$verify_script" --definitely-unknown

# Build a fake repo tree: minimal assets, web/dist marker, READMEs, and a
# stub CMake that implements the two install components the script uses.
fixture="$temporary_root/fixture"
mkdir -p "$fixture/assets/models_dev" "$fixture/assets/seed/skills/demo" \
    "$fixture/web/dist" "$fixture/build" "$fixture/scripts"
printf 'models api\n' >"$fixture/assets/models_dev/api.json"
printf 'manifest\n' >"$fixture/assets/models_dev/MANIFEST.json"
printf 'license\n' >"$fixture/assets/models_dev/LICENSE"
printf 'seed skill\n' >"$fixture/assets/seed/skills/demo/SKILL.md"
printf '{"bundle":"test"}\n' >"$fixture/assets/seed/MANIFEST.json"
printf '1\n' >"$fixture/assets/seed/seed.version"
printf '<html></html>\n' >"$fixture/web/dist/index.html"
printf 'readme\n' >"$fixture/README.md"
printf 'readme cn\n' >"$fixture/README_CN.md"
printf 'cmake cache marker\n' >"$fixture/build/CMakeCache.txt"
cp "$repo_root/scripts/verify_seed_bundle.py" "$fixture/scripts/verify_seed_bundle.py"

# Negative: --skip-build without a configured build dir fails the preflight.
rm -rf "$temporary_root/nobuild"
mkdir -p "$temporary_root/nobuild"
expect_status 1 "unconfigured build dir" "$python_bin" "$verify_script" \
    --skip-build --platform linux --target tui \
    --repo "$fixture" --build-dir "$temporary_root/nobuild" \
    --staging-dir "$fixture/staging"
expect_output "not configured" "unconfigured build dir detail"

# Negative: missing web/dist fails with the rebuild command.
rm "$fixture/web/dist/index.html"
expect_status 1 "missing web dist" "$python_bin" "$verify_script" \
    --skip-build --platform linux --target tui \
    --repo "$fixture" --build-dir "$fixture/build" \
    --staging-dir "$fixture/staging"
expect_output "pnpm build" "web dist rebuild hint"
printf '<html></html>\n' >"$fixture/web/dist/index.html"

if [[ "$full_mode" -eq 1 ]]; then
    mkdir -p "$temporary_root/shim"
    cat >"$temporary_root/shim/cmake" <<'EOF'
#!/bin/sh
set -eu
mode=""
installdir=""
prefix=""
component=""
while [ $# -gt 0 ]; do
    case "$1" in
        --install) mode="install"; installdir="$2"; shift ;;
        --prefix) prefix="$2"; shift ;;
        --component) component="$2"; shift ;;
    esac
    shift
done
if [ "$mode" = "install" ]; then
    repo="$(cd "$installdir/.." && pwd)"
    case "$component" in
        models_dev_registry)
            mkdir -p "$prefix/share/acecode/models_dev"
            cp "$repo"/assets/models_dev/* "$prefix/share/acecode/models_dev/"
            ;;
        default_seed_bundle)
            rm -rf "$prefix/share/acecode/seed"
            cp -R "$repo/assets/seed" "$prefix/share/acecode/seed"
            ;;
    esac
fi
exit 0
EOF
    chmod +x "$temporary_root/shim/cmake"
    PATH="$temporary_root/shim:$PATH"
    export PATH

    write_tui_stub() {
        local registry_rc="$1"
        cat >"$fixture/build/acecode" <<EOF
#!/bin/sh
case "\$1" in
    --version) echo "acecode v0.0.0-test"; exit 0 ;;
    --validate-models-registry)
        if [ "$registry_rc" = "0" ]; then
            echo "models.dev registry OK: 2 providers, 9 models, source=$fixture/build/share/acecode/models_dev/api.json"
            exit 0
        fi
        echo "models.dev registry not found or empty" >&2
        exit 1 ;;
esac
exit 0
EOF
        chmod +x "$fixture/build/acecode"
    }

    write_desktop_stub() {
        cat >"$fixture/build/acecode-desktop" <<'EOF'
#!/bin/sh
sleep 300
EOF
        chmod +x "$fixture/build/acecode-desktop"
        cat >"$fixture/build/acecode-daemon" <<'EOF'
#!/bin/sh
exit 0
EOF
        chmod +x "$fixture/build/acecode-daemon"
    }

    # Case: TUI-only flat (linux) flow passes end to end.
    write_tui_stub 0
    expect_status 0 "tui flow" "$python_bin" "$verify_script" \
        --skip-build --platform linux --target tui \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging"
    expect_output "verify-package: PASS" "tui flow summary"
    [[ -f "$fixture/staging/acecode" ]]
    [[ -f "$fixture/staging/share/acecode/models_dev/api.json" ]]
    [[ -f "$fixture/staging/share/acecode/seed/skills/demo/SKILL.md" ]]

    # Case: full linux flow including desktop launch and daemon adjacency.
    write_desktop_stub
    expect_status 0 "linux full flow" "$python_bin" "$verify_script" \
        --skip-build --platform linux --target all \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging" --launch-timeout 2
    expect_output "\[PASS\] desktop daemon adjacency" "daemon adjacency"
    expect_output "\[PASS\] desktop launch" "desktop launch"

    # Case: macOS app bundle flow verifies bundle layout and resources.
    mkdir -p "$fixture/build/ACECode.app/Contents/MacOS" \
        "$fixture/build/ACECode.app/Contents/Resources/share/acecode/models_dev" \
        "$fixture/build/ACECode.app/Contents/Resources/share/acecode/seed"
    cat >"$fixture/build/ACECode.app/Contents/MacOS/ACECode" <<'EOF'
#!/bin/sh
sleep 300
EOF
    chmod +x "$fixture/build/ACECode.app/Contents/MacOS/ACECode"
    cat >"$fixture/build/ACECode.app/Contents/MacOS/acecode-daemon" <<'EOF'
#!/bin/sh
exit 0
EOF
    chmod +x "$fixture/build/ACECode.app/Contents/MacOS/acecode-daemon"
    cp "$fixture/assets/models_dev/"* \
        "$fixture/build/ACECode.app/Contents/Resources/share/acecode/models_dev/"
    # The destination already exists (created by mkdir -p above), so plain
    # `cp -R src dst` nests the tree as dst/seed instead of filling dst, which
    # makes the packaged seed layout mismatch assets/seed. Copy the contents.
    cp -R "$fixture/assets/seed/." \
        "$fixture/build/ACECode.app/Contents/Resources/share/acecode/seed/"
    expect_status 0 "darwin bundle flow" "$python_bin" "$verify_script" \
        --skip-build --platform darwin --target desktop \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging" --launch-timeout 2
    expect_output "\[PASS\] app bundle acecode-daemon" "bundle daemon"
    expect_output "\[PASS\] models_dev registry (app bundle)" "bundle models_dev"

    # Negative: mutated models.dev file set fails.
    printf 'extra\n' >"$fixture/assets/models_dev/extra.json"
    expect_status 1 "models_dev file count" "$python_bin" "$verify_script" \
        --skip-build --platform linux --target tui \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging"
    expect_output "expected exactly" "models_dev count detail"
    rm "$fixture/assets/models_dev/extra.json"

    # Negative: failing registry validation fails the run.
    write_tui_stub 1
    expect_status 1 "registry validation failure" "$python_bin" "$verify_script" \
        --skip-build --platform linux --target tui \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging"
    expect_output "tui models registry resolution" "registry failure item"

    # Negative: desktop exiting immediately fails the launch probe.
    write_tui_stub 0
    cat >"$fixture/build/acecode-desktop" <<'EOF'
#!/bin/sh
exit 0
EOF
    chmod +x "$fixture/build/acecode-desktop"
    expect_status 1 "desktop immediate exit" "$python_bin" "$verify_script" \
        --skip-build --platform linux --target desktop \
        --repo "$fixture" --build-dir "$fixture/build" \
        --staging-dir "$fixture/staging"
    expect_output "exited immediately" "desktop exit detail"
else
    echo "verify_package_test: flow cases skipped on non-POSIX host"
fi

echo "verify_package_test: OK"
