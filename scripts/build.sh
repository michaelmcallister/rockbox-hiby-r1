#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/work}"
UPSTREAM_DIR="${UPSTREAM_DIR:-$WORK_DIR/rockbox}"
PATCH_DIR="${PATCH_DIR:-$ROOT_DIR/patches}"
OVERLAY_DIR="${OVERLAY_DIR:-$ROOT_DIR/overlay}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out}"
CCACHE_DIR="${CCACHE_DIR:-$ROOT_DIR/.ccache}"

ROCKBOX_REMOTE="${ROCKBOX_REMOTE:-https://github.com/Rockbox/rockbox.git}"
ROCKBOX_BASE_COMMIT="${ROCKBOX_BASE_COMMIT:-9a6e3799e18ba1e365cb23f6a2a5044e49cecffc}"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-$ROOT_DIR/toolchain}"
RBDEV_DOWNLOAD="${RBDEV_DOWNLOAD:-$WORK_DIR/rbdev-download}"
RBDEV_BUILD="${RBDEV_BUILD:-$WORK_DIR/rbdev-build}"
GNU_MIRROR="${GNU_MIRROR:-https://ftp.gnu.org/gnu}"
LINUX_MIRROR="${LINUX_MIRROR:-https://cdn.kernel.org/pub/linux}"

log() {
  printf '[build] %s\n' "$*"
}

install_overlay() {
  local src rel dst

  [ -d "$OVERLAY_DIR" ] || return 0

  while IFS= read -r src; do
    rel="${src#$OVERLAY_DIR/}"
    dst="$UPSTREAM_DIR/$rel"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
  done < <(find "$OVERLAY_DIR" -type f | sort)
}

default_build_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
  else
    echo 1
  fi
}

sha256_cmd() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"
  fi
}

toolchain_smoke_test() {
  local tmpdir cfile ofile

  tmpdir="$(mktemp -d)"
  cfile="$tmpdir/toolchain-smoke.c"
  ofile="$tmpdir/toolchain-smoke.o"
  printf 'int toolchain_smoke;\n' > "$cfile"

  if "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" -c "$cfile" -o "$ofile" >/dev/null 2>&1; then
    rm -rf "$tmpdir"
    return 0
  fi

  rm -rf "$tmpdir"
  return 1
}

toolchain_ready() {
  [ -x "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" ] &&
    [ -f "$TOOLCHAIN_PREFIX/mipsel-rockbox-linux-gnu/sysroot/usr/include/sys/types.h" ] &&
    toolchain_smoke_test
}

setup_ccache() {
  local wrapper_dir

  if ! command -v ccache >/dev/null 2>&1; then
    return
  fi

  wrapper_dir="$WORK_DIR/ccache-wrappers"
  mkdir -p "$wrapper_dir" "$CCACHE_DIR"

  cat > "$wrapper_dir/mipsel-rockbox-linux-gnu-gcc" <<EOF
#!/usr/bin/env bash
exec ccache "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" "\$@"
EOF
  chmod +x "$wrapper_dir/mipsel-rockbox-linux-gnu-gcc"

  cat > "$wrapper_dir/mipsel-rockbox-linux-gnu-g++" <<EOF
#!/usr/bin/env bash
exec ccache "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-g++" "\$@"
EOF
  chmod +x "$wrapper_dir/mipsel-rockbox-linux-gnu-g++"

  export CCACHE_DIR
  export CCACHE_BASEDIR="$ROOT_DIR"
  export CCACHE_COMPILERCHECK=content
  export CCACHE_NOHASHDIR=1
  export PATH="$wrapper_dir:$TOOLCHAIN_PREFIX/bin:$PATH"

  log "ccache enabled: $CCACHE_DIR"
  ccache --zero-stats >/dev/null 2>&1 || true
}

BUILD_JOBS="${BUILD_JOBS:-$(default_build_jobs)}"

mkdir -p "$WORK_DIR" "$OUT_DIR"

if [ ! -d "$UPSTREAM_DIR/.git" ]; then
  log "initializing upstream Rockbox repo"
  git init "$UPSTREAM_DIR"
fi

# Containerized/local repros may mount the workspace with a different owner.
git config --global --add safe.directory "$UPSTREAM_DIR" >/dev/null 2>&1 || true

git -C "$UPSTREAM_DIR" remote remove origin >/dev/null 2>&1 || true
git -C "$UPSTREAM_DIR" remote add origin "$ROCKBOX_REMOTE"

log "fetching upstream base commit $ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" fetch --force --depth 1 origin "$ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" checkout --force FETCH_HEAD
git -C "$UPSTREAM_DIR" clean -fdx
git -C "$UPSTREAM_DIR" config user.name "rockbox-hiby-r1-ci"
git -C "$UPSTREAM_DIR" config user.email "ci@example.invalid"
(git -C "$UPSTREAM_DIR" am --abort >/dev/null 2>&1 || true)

log "installing overlay files"
install_overlay

PATCHES=()
while IFS= read -r patch; do
  PATCHES+=("$patch")
done < <(find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' | sort)

if [ ${#PATCHES[@]} -eq 0 ]; then
  echo "No patches found in $PATCH_DIR" >&2
  exit 1
fi

log "applying ${#PATCHES[@]} patches"
for p in "${PATCHES[@]}"; do
  log "git am $(basename "$p")"
  git -C "$UPSTREAM_DIR" am "$p"
done

PATCHED_HEAD="$(git -C "$UPSTREAM_DIR" rev-parse --verify HEAD)"

if ! toolchain_ready; then
  if [ -d "$TOOLCHAIN_PREFIX" ]; then
    log "existing toolchain is unusable; rebuilding"
    rm -rf "$TOOLCHAIN_PREFIX"
  fi
  rm -rf "$RBDEV_BUILD"
  export RBDEV_PREFIX="$TOOLCHAIN_PREFIX"
  export RBDEV_DOWNLOAD
  export RBDEV_BUILD
  export GNU_MIRROR
  export LINUX_MIRROR
  log "bootstrapping toolchain via rockboxdev.sh"
  "$UPSTREAM_DIR/tools/rockboxdev.sh" --target=y
fi

setup_ccache

if ! command -v mipsel-rockbox-linux-gnu-gcc >/dev/null 2>&1; then
  export PATH="$TOOLCHAIN_PREFIX/bin:$PATH"
fi

FW_BUILD_DIR="$WORK_DIR/build-hibyr1-rockbox"
BL_BUILD_DIR="$WORK_DIR/build-hibyr1-bootloader"

rm -rf "$FW_BUILD_DIR" "$BL_BUILD_DIR"
mkdir -p "$FW_BUILD_DIR" "$BL_BUILD_DIR"

log "building firmware"
(
  cd "$FW_BUILD_DIR"
  "$UPSTREAM_DIR/tools/configure" --target=hibyr1 --type=n
  make -j"$BUILD_JOBS"
)

log "building bootloader"
(
  cd "$BL_BUILD_DIR"
  "$UPSTREAM_DIR/tools/configure" --target=hibyr1 --type=b
  make -j"$BUILD_JOBS"
)

cp -f "$FW_BUILD_DIR/rockbox.r1" "$OUT_DIR/"
cp -f "$FW_BUILD_DIR/rockbox-info.txt" "$OUT_DIR/"
[ -f "$FW_BUILD_DIR/rockbox.zip" ] && cp -f "$FW_BUILD_DIR/rockbox.zip" "$OUT_DIR/"

if [ -f "$BL_BUILD_DIR/bootloader.r1" ]; then
  cp -f "$BL_BUILD_DIR/bootloader.r1" "$OUT_DIR/"
else
  # fallback in case naming changes
  find "$BL_BUILD_DIR" -maxdepth 1 -type f -name 'bootloader*' -exec cp -f {} "$OUT_DIR/" \;
fi

cat > "$OUT_DIR/build-metadata.txt" <<META
rockbox_remote=$ROCKBOX_REMOTE
rockbox_base_commit=$ROCKBOX_BASE_COMMIT
patched_head=$PATCHED_HEAD
build_jobs=$BUILD_JOBS
gnu_mirror=$GNU_MIRROR
linux_mirror=$LINUX_MIRROR
META

(
  cd "$OUT_DIR"
  sha256_cmd $(ls -1) > SHA256SUMS
)

log "build complete; artifacts in $OUT_DIR"
ls -l "$OUT_DIR"

if command -v ccache >/dev/null 2>&1; then
  ccache --show-stats || true
fi
