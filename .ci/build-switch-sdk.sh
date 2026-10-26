#!/usr/bin/env bash
# Linux CI equivalent of mesa-switch's unified SDK workflow. Run as root in
# devkitpro/devkita64; this installs host tools and modifies container portlibs.
# Recipe: https://github.com/danfromtico/mesa-switch/blob/main/.github/workflows/build.yml
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="$ROOT/build-switch-deps"
PREPARE="$ROOT/scripts/prepare-switch-vulkan-sdk.sh"
SDK_VERSION="$(sed -n 's/^SDK_VERSION=//p' "$PREPARE")"
SDK_REVISION="$(sed -n 's/^SDK_REVISION=//p' "$PREPARE")"
[[ "$SDK_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
[[ "$SDK_REVISION" =~ ^[0-9a-f]{40}$ ]]
SDK="$BUILD_ROOT/mesa-$SDK_VERSION-switch-unified-horizon-sdk"
SOURCE="$BUILD_ROOT/mesa-source"
SDK_PATCHES=(
    "$ROOT/.ci/patches/mesa-nvk-alloca.patch"
    "$ROOT/.ci/patches/mesa-optional-libelf.patch"
)
JOBS="${VITA3K_BUILD_JOBS:-2}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export CARGO_HOME=/root/.cargo RUSTUP_HOME=/root/.rustup
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export PATH="$CARGO_HOME/bin:$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

if [[ -d "$SDK" ]]; then
    bash "$PREPARE" --check
    exit
fi
[[ "$(uname -s)" == Linux && "$EUID" == 0 ]] || {
    echo 'Run this CI helper as root inside the devkitpro/devkita64 Linux container.' >&2
    exit 1
}

apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config flex bison curl ca-certificates \
    git xz-utils clang libclang-dev llvm-15-dev libclang-15-dev \
    libclang-common-15-dev libclc-15-dev libllvmspirvlib-15-dev \
    python3-pip python3-setuptools python3-wheel
pip3 install --break-system-packages --no-cache-dir meson mako pyyaml packaging

# Use the packages baked into the official image. GitHub runners cannot fetch
# packages from devkitPro, including reinstalling Mesa after the SDK build.
dkp-pacman -Q switch-mesa switch-pkg-config switch-zlib switch-libzstd \
    switch-libexpat switch-libdrm_nouveau

mkdir -p "$BUILD_ROOT"

# Hide the packaged GL implementation during configuration, then restore the
# exact same files for the launcher, including when the SDK build fails.
# Querying the local package manifest leaves the package database unchanged.
mesa_backup="$(mktemp -d "$BUILD_ROOT/mesa-portlib.XXXXXX")"
dkp-pacman -Qlq switch-mesa > "$mesa_backup/files"
mapfile -t mesa_files < "$mesa_backup/files"
restore_packaged_mesa() {
    local path saved
    for path in "${mesa_files[@]}"; do
        saved="$mesa_backup$path"
        if [[ -f "$saved" || -L "$saved" ]]; then
            mkdir -p "$(dirname "$path")"
            mv -- "$saved" "$path" || return
        fi
    done
}
trap restore_packaged_mesa EXIT
for path in "${mesa_files[@]}"; do
    [[ -f "$path" || -L "$path" ]] || continue
    [[ "$path" == "$DEVKITPRO/portlibs/switch/"* ]] || {
        echo "Unexpected switch-mesa package file: $path" >&2
        exit 1
    }
    mkdir -p "$mesa_backup$(dirname "$path")"
    mv -- "$path" "$mesa_backup$path"
done

git init "$SOURCE"
git -C "$SOURCE" fetch --depth 1 https://github.com/danfromtico/mesa-switch.git "$SDK_REVISION"
git -C "$SOURCE" checkout --detach FETCH_HEAD
test "$(git -C "$SOURCE" rev-parse HEAD)" = "$SDK_REVISION"
test "$(cat "$SOURCE/VERSION")" = "$SDK_VERSION"

# Keep the pinned revision with the CI portability and SDK metadata fixes.
for patch in "${SDK_PATCHES[@]}"; do
    git -C "$SOURCE" apply --check "$patch"
    git -C "$SOURCE" apply "$patch"
done

# Debian's SPIRV-Tools is too old for mesa_clc. Use the version in Docker.rust.
SPIRV_TAG=vulkan-sdk-1.3.290.0
SPIRV_SOURCE="$BUILD_ROOT/spirv-tools"
SPIRV_PREFIX="$BUILD_ROOT/spirv-tools-install"
git clone --depth 1 --branch "$SPIRV_TAG" \
    https://github.com/KhronosGroup/SPIRV-Tools.git "$SPIRV_SOURCE"
git clone --depth 1 --branch "$SPIRV_TAG" \
    https://github.com/KhronosGroup/SPIRV-Headers.git "$SPIRV_SOURCE/external/spirv-headers"
cmake -S "$SPIRV_SOURCE" -B "$SPIRV_SOURCE/build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$SPIRV_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TYPE=Release -DSPIRV_SKIP_TESTS=ON
cmake --build "$SPIRV_SOURCE/build" --parallel "$JOBS"
cmake --install "$SPIRV_SOURCE/build"

# Match upstream's nightly toolchain requirement; record the actual versions
# in the cached SDK so artifacts show which compiler produced the libraries.
curl --proto '=https' --tlsv1.2 -fsSL --retry 3 https://sh.rustup.rs -o "$BUILD_ROOT/rustup-init.sh"
sh "$BUILD_ROOT/rustup-init.sh" -y --default-toolchain nightly --profile minimal
rustup component add --toolchain nightly rust-src rustfmt
rustup target add "$MESA_SWITCH_RUST_TARGET"
cargo install --locked --jobs "$JOBS" bindgen-cli cbindgen

mkdir -p /usr/local/libexec
install -m 755 "$SOURCE/bindgen-switch-wrapper.sh" /usr/local/libexec/bindgen
install -m 755 "$SOURCE/rustc-switch-wrapper.sh" /usr/local/libexec/rustc
install -m 644 "$SOURCE/bindgen-atomic-shim.h" /usr/local/libexec/bindgen-atomic-shim.h

# Keep the Switch nouveau.h API, not Mesa's incompatible Linux DRM facade.
inc="$DEVKITPRO/portlibs/switch/include"
if ! grep -q 'struct nouveau_list' "$inc/nouveau.h"; then
    echo 'The container must provide the switch-libdrm_nouveau nouveau.h header.' >&2
    exit 1
fi
install -m 644 "$SOURCE/src/nouveau/headers/nv_device_info.h" "$inc/"

llvm_config=/usr/bin/llvm-config-15
libdir="$("$llvm_config" --libdir)"
version="$("$llvm_config" --version)"
major="${version%%.*}"
res="$libdir/clang/$version/include"
if [[ ! -d "$res" ]]; then
    res="$libdir/clang/$major/include"
fi
test -f "$res/opencl-c-base.h"
if [[ ! -e "$libdir/clang/$version" ]]; then
    ln -s "$major" "$libdir/clang/$version"
fi
if [[ ! -e "$libdir/clang/$major" ]]; then
    ln -s "$version" "$libdir/clang/$major"
fi
printf "[binaries]\nllvm-config = '%s'\n" "$llvm_config" > "$BUILD_ROOT/llvm-native.txt"

# Rust std requests these archives; Mesa's rust_switch_stubs.c provides symbols.
printf 'void __mesa_switch_stub_lib(void) {}\n' > "$BUILD_ROOT/stub_posix.c"
"$DEVKITA64/bin/aarch64-none-elf-gcc" -c "$BUILD_ROOT/stub_posix.c" -o "$BUILD_ROOT/stub_posix.o"
for lib in dl rt util; do
    if [[ ! -f "$DEVKITPRO/portlibs/switch/lib/lib$lib.a" ]]; then
        "$DEVKITA64/bin/aarch64-none-elf-ar" rcs \
            "$DEVKITPRO/portlibs/switch/lib/lib$lib.a" "$BUILD_ROOT/stub_posix.o"
    fi
done

export PKG_CONFIG_PATH="$SPIRV_PREFIX/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$SPIRV_PREFIX"
cd "$SOURCE"
SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
export SOURCE_DATE_EPOCH ZERO_AR_DATE=1
meson setup builddir-native --native-file "$BUILD_ROOT/llvm-native.txt" --buildtype=release \
    -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=enabled -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    '-Dtools=[]' -Dllvm=enabled -Dmesa-clc=enabled -Dmesa-clc-bundle-headers=enabled \
    -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
ninja -C builddir-native -j "$JOBS" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
export PATH="$SOURCE/builddir-native/src/compiler/clc:$SOURCE/builddir-native/src/compiler/spirv:$PATH"

meson setup builddir-switch --cross-file switch_cross_file.txt \
    --default-library=static --prefix=/opt/devkitpro/portlibs/switch --libdir=lib \
    --buildtype=release -Doptimization=2 -Db_lto=false -Db_ndebug=true \
    -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau,zink -Dgallium-rusticl=false \
    -Dplatforms=switch -Degl-native-platform=switch -Dglx=disabled -Degl=enabled \
    -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Dvideo-codecs= \
    -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled '-Dtools=[]' \
    -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled \
    -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dbuild-tests=false \
    -Dnvk-build-id="$SDK_REVISION"
ninja -C builddir-switch -j "$JOBS"
meson install -C builddir-switch --destdir "$BUILD_ROOT/mesa-stage"
staged="$BUILD_ROOT/mesa-stage/opt/devkitpro/portlibs/switch"
for pc in "$staged"/lib/pkgconfig/*.pc; do
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done

# This is a source build of the pinned revision, not the unpublished ZIP.
# Record its real file hashes; never claim it has SDK_ARCHIVE_SHA256.
mkdir -p "$staged/share/mesa-switch"
printf '%s\n' "$SDK_REVISION" > "$staged/share/mesa-switch/revision"
{
    printf 'Mesa source: https://github.com/danfromtico/mesa-switch\n'
    printf 'Mesa version: %s\nSPIRV-Tools tag: %s\n' "$SDK_VERSION" "$SPIRV_TAG"
    printf 'Mesa source patches (SHA256):\n'
    (cd "$ROOT" && sha256sum .ci/patches/mesa-*.patch)
    rustc --version
    bindgen --version
    cbindgen --version
    meson --version
    "$DEVKITA64/bin/aarch64-none-elf-gcc" --version
} > "$staged/share/mesa-switch/build-info.txt"
(
    cd "$staged"
    find . -type f ! -path './share/mesa-switch/sha256sums' -print0 \
        | sort -z | xargs -0 sha256sum > share/mesa-switch/sha256sums
)
mv "$staged" "$SDK"
bash "$PREPARE" --check
restore_packaged_mesa
trap - EXIT
