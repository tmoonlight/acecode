#!/usr/bin/env bash
# Build a macOS "green"/portable (install-free) package for ACECode.
#
# Layout mirrors CI `.github/workflows/package.yml` "Package (Unix)" macOS branch:
#   acecode (TUI) + README.md + README_CN.md + share/acecode/{models_dev,seed} + ACECode.app
# Zipped with ditto (keeps macOS metadata + symlinks, safe for .app).
#
# WHY THIS SCRIPT EXISTS (a real footgun):
#   Front-end assets are embedded into C++ only at *cmake configure* time
#   (cmake/acecode_embed_assets.cmake -> build/.../generated/static_assets_data.cpp).
#   Running `cmake --build` alone WILL NOT refresh them. If you edit web/ and just
#   rebuild, the zip ships yesterday's UI silently. This script guards against that.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir=""
arch="$(uname -m)"
suffix="portable"
output_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--arch|--suffix|--output)
            [[ $# -ge 2 && -n "$2" && "$2" != -* ]] || {
                echo "ERROR: $1 requires a value" >&2; exit 2;
            } ;;
    esac
    case "$1" in
        --build-dir) build_dir="${2:-}"; shift 2 ;;
        --arch) arch="${2:-}"; shift 2 ;;
        --suffix) suffix="${2:-}"; shift 2 ;;
        --output) output_path="${2:-}"; shift 2 ;;
        --help|-h) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 2 ;;
        *) suffix="$1"; shift ;;   # first positional arg = suffix
    esac
done

case "$arch" in
    x86_64|x64) arch=x86_64; triplet=x64-osx; build_arch=x64 ;;
    arm64|aarch64) arch=arm64; triplet=arm64-osx; build_arch=arm64 ;;
    *) echo "ERROR: unsupported architecture: $arch" >&2; exit 2 ;;
esac
[[ -n "$build_dir" ]] || build_dir="$repo_root/build/macos-$build_arch-release"
build_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve().as_posix())' "$build_dir")"

# An explicit build directory must not reuse libraries from another target.
cache_file="$build_dir/CMakeCache.txt"
if [[ -f "$cache_file" ]]; then
    cached_arch="$(sed -n 's/^CMAKE_OSX_ARCHITECTURES:[^=]*=//p' "$cache_file")"
    cached_triplet="$(sed -n 's/^VCPKG_TARGET_TRIPLET:[^=]*=//p' "$cache_file")"
    if [[ ( -n "$cached_arch" && "$cached_arch" != "$arch" ) ||
          ( -n "$cached_triplet" && "$cached_triplet" != "$triplet" ) ]]; then
        echo "ERROR: build cache architecture/triplet does not match $arch/$triplet; use a separate --build-dir" >&2
        exit 1
    fi
fi

version="$(grep -m1 'project(acecode VERSION' "$repo_root/CMakeLists.txt" | sed -E 's/.* VERSION ([0-9.]+).*/\1/')"
dist_html="$repo_root/web/dist/index.html"
embed_cpp="$build_dir/generated/static_assets_data.cpp"
embed_marker="$build_dir/generated/embedded_web_dist.sha"
package_name="acecode-macos-$arch"
package_dir="$repo_root/dist/$package_name"
[[ -z "$output_path" ]] && output_path="$repo_root/dist/acecode-${version}-macos-${arch}-${suffix}-portable.zip"
output_path="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve().as_posix())' "$output_path")"
DITTO_BIN="${DITTO_BIN:-/usr/bin/ditto}"
LIPO_BIN="${LIPO_BIN:-/usr/bin/lipo}"

echo "== version : $version"
echo "== branch  : $(git -C "$repo_root" rev-parse --abbrev-ref HEAD) @ $(git -C "$repo_root" rev-parse --short HEAD)"
echo "== build   : $build_dir"

# Locate a working cmake. The wrapper at ~/.local/bin/cmake can be a broken
# pip shim; the real binary lives under the cmake package's data/bin/. Prefer
# any candidate that answers --version.
requested_cmake="${CMAKE_BIN:-}"
CMAKE_BIN=""
for cand in \
    "$requested_cmake" \
    "$(command -v cmake 2>/dev/null || true)" \
    "$(find "$HOME/.local/lib" -path '*/cmake/data/bin/cmake' -type f 2>/dev/null | head -1 || true)"; do
    [[ -n "$cand" && -x "$cand" ]] || continue
    if "$cand" --version >/dev/null 2>&1; then CMAKE_BIN="$cand"; break; fi
done
[[ -n "$CMAKE_BIN" ]] || { echo "ERROR: cmake not found" >&2; exit 1; }
echo "== cmake   : $CMAKE_BIN"

# ---------------------------------------------------------------------------
# GUARD 1: front-end source newer than build output? -> forgot `pnpm build`
# ---------------------------------------------------------------------------
if [[ ! -f "$dist_html" ]]; then
    echo "ERROR: web/dist/index.html missing — run 'cd web && pnpm install && pnpm build' first." >&2
    exit 1
fi
newest_src="$(find "$repo_root/web/src" -type f -newer "$dist_html" 2>/dev/null | head -1 || true)"
if [[ -n "$newest_src" ]]; then
    echo "ERROR: front-end source is newer than web/dist (e.g. $newest_src)." >&2
    echo "       You edited web/ but did not rebuild. Run:" >&2
    echo "         cd web && pnpm install && pnpm build" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# GUARD 2: dist newer than embedded assets? -> forgot to reconfigure cmake.
# Decision is made by BOTH mtime AND a recorded dist hash marker (mtime alone
# can lie after a git checkout/reset rewrites the tree).
# ---------------------------------------------------------------------------
needs_reconfigure=false
if [[ ! -f "$cache_file" || ! -f "$embed_cpp" || "${cached_arch:-}" != "$arch" || "${cached_triplet:-}" != "$triplet" ]]; then
    needs_reconfigure=true
elif [[ "$dist_html" -nt "$embed_cpp" ]]; then
    needs_reconfigure=true
fi
current_hash="$(python3 - "$repo_root/web/dist" <<'PY'
import hashlib
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
digest = hashlib.sha256()
for path in sorted(p for p in root.rglob('*') if p.is_file()):
    digest.update(path.relative_to(root).as_posix().encode())
    digest.update(b'\0')
    digest.update(path.read_bytes())
    digest.update(b'\0')
print(digest.hexdigest())
PY
)"
if [[ -f "$embed_marker" ]]; then
    embedded_hash="$(cat "$embed_marker" 2>/dev/null || true)"
    [[ "$embedded_hash" != "$current_hash" ]] && needs_reconfigure=true
else
    needs_reconfigure=true
fi

if $needs_reconfigure; then
    echo "== web/dist changed since last embed — reconfiguring cmake (skipping vcpkg install) =="
    "$CMAKE_BIN" -S "$repo_root" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-$HOME/vcpkg}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="$triplet" \
        -DVCPKG_OVERLAY_PORTS="$repo_root/ports" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DBUILD_TESTING=OFF -DACECODE_BUILD_DESKTOP=ON \
        -DVCPKG_MANIFEST_FEATURES=tests -DVCPKG_MANIFEST_INSTALL=OFF
else
    echo "== embedded assets already fresh (dist hash matches $embed_marker) =="
fi

# The frontend marker only controls reconfiguration. Backend changes must
# always reach the package, even when all frontend inputs are unchanged.
echo "== incrementally building current binaries =="
"$CMAKE_BIN" --build "$build_dir" --target acecode-desktop
echo "$current_hash" > "$embed_marker"

for binary in "$build_dir/acecode" \
    "$build_dir/acecode-computer-use" \
    "$build_dir/acecode-ios-sim" \
    "$build_dir/ACECode.app/Contents/MacOS/ACECode" \
    "$build_dir/ACECode.app/Contents/MacOS/acecode-daemon" \
    "$build_dir/ACECode.app/Contents/MacOS/acecode-computer-use" \
    "$build_dir/ACECode.app/Contents/MacOS/acecode-ios-sim"; do
    if [[ ! -x "$binary" ]] || ! "$LIPO_BIN" "$binary" -verify_arch "$arch"; then
        echo "ERROR: missing executable or wrong architecture ($arch): $binary" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Assemble portable layout
# ---------------------------------------------------------------------------
validate_models_dev_registry() {
    local registry_dir="$1"
    local file_count
    file_count="$(find "$registry_dir" -type f | wc -l | tr -d '[:space:]')"
    if [[ "$file_count" != "3" ]]; then
        echo "Expected exactly 3 models.dev files in $registry_dir, found $file_count" >&2
        exit 1
    fi
    for f in api.json MANIFEST.json LICENSE; do
        if [[ ! -f "$registry_dir/$f" ]] ||
           ! cmp -s "$repo_root/assets/models_dev/$f" "$registry_dir/$f"; then
            echo "Missing or mismatched models.dev file: $registry_dir/$f" >&2
            exit 1
        fi
    done
}

echo "== assembling $package_dir =="
if [[ -d "$package_dir" ]]; then
    mv "$package_dir" "/tmp/portable-pkg-$(date +%s)"
fi
mkdir -p "$package_dir"

if [[ ! -f "$build_dir/acecode" ]]; then
    echo "ERROR: terminal executable missing: $build_dir/acecode (run the build above again)" >&2
    exit 1
fi
cp "$build_dir/acecode" "$package_dir/"
"$CMAKE_BIN" --install "$build_dir" --prefix "$package_dir" --component computer_use_runtime >/dev/null
"$CMAKE_BIN" --install "$build_dir" --prefix "$package_dir" --component ios_simulator_runtime >/dev/null
cp "$repo_root/README.md" "$repo_root/README_CN.md" "$package_dir/"

"$CMAKE_BIN" --install "$build_dir" --prefix "$package_dir" --component models_dev_registry >/dev/null
"$CMAKE_BIN" --install "$build_dir" --prefix "$package_dir" --component default_seed_bundle >/dev/null
validate_models_dev_registry "$package_dir/share/acecode/models_dev"
python3 "$repo_root/scripts/verify_seed_bundle.py" \
    --source "$repo_root/assets/seed" \
    --packaged "$package_dir/share/acecode/seed"

if [[ ! -d "$build_dir/ACECode.app" ]]; then
    echo "ERROR: desktop app bundle missing: $build_dir/ACECode.app" >&2
    exit 1
fi
cp -R "$build_dir/ACECode.app" "$package_dir/"

legacy="$(find "$package_dir" -maxdepth 1 -iname 'ace-browser-*' -print -quit)"
if [[ -n "$legacy" ]]; then
    echo "ERROR: legacy browser artifact must not be packaged: $legacy" >&2
    exit 1
fi

echo "== zipping with ditto =="
mkdir -p "$(dirname "$output_path")"
rm -f "$output_path"
( cd "$repo_root/dist" && "$DITTO_BIN" -c -k --keepParent --sequesterRsrc \
    "$package_name" "$output_path" )

# ---------------------------------------------------------------------------
# GUARD 3: verify the freshly built binary actually contains embedded UI.
# `strings` drops non-ASCII, so grep the binary directly for a dist-only token.
# ---------------------------------------------------------------------------
echo "== verifying archive =="
verify_root="$(mktemp -d)"
trap 'rm -rf -- "$verify_root"' EXIT
"$DITTO_BIN" -x -k "$output_path" "$verify_root"
extracted="$verify_root/$package_name"

[[ -x "$extracted/acecode" ]] || { echo "extracted acecode not executable" >&2; exit 1; }
[[ -x "$extracted/acecode-computer-use" ]] || { echo "extracted Computer Use helper missing" >&2; exit 1; }
[[ -x "$extracted/acecode-ios-sim" ]] || { echo "extracted iOS Simulator helper missing" >&2; exit 1; }
[[ -x "$extracted/ACECode.app/Contents/MacOS/ACECode" ]] || { echo "extracted app main missing" >&2; exit 1; }
[[ -x "$extracted/ACECode.app/Contents/MacOS/acecode-daemon" ]] || { echo "extracted daemon missing" >&2; exit 1; }
[[ -x "$extracted/ACECode.app/Contents/MacOS/acecode-computer-use" ]] || { echo "extracted app Computer Use helper missing" >&2; exit 1; }
[[ -x "$extracted/ACECode.app/Contents/MacOS/acecode-ios-sim" ]] || { echo "extracted app iOS Simulator helper missing" >&2; exit 1; }
if ! grep -aq "provider-logos" "$extracted/acecode"; then
    echo "ERROR: built binary does NOT contain embedded web UI (provider-logos absent)." >&2
    echo "       The embed step silently fell back to an empty asset map. Reconfigure cmake." >&2
    exit 1
fi
validate_models_dev_registry "$extracted/share/acecode/models_dev"
validate_models_dev_registry "$extracted/ACECode.app/Contents/Resources/share/acecode/models_dev"
python3 "$repo_root/scripts/verify_seed_bundle.py" \
    --source "$repo_root/assets/seed" --packaged "$extracted/share/acecode/seed"
python3 "$repo_root/scripts/verify_seed_bundle.py" \
    --source "$repo_root/assets/seed" \
    --packaged "$extracted/ACECode.app/Contents/Resources/share/acecode/seed"

echo "== done =="
du -sh "$package_dir" | cat
ls -lh "$output_path" | cat
shasum -a 256 "$output_path" | cat
echo "OK: $output_path"
