#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/work}"
UPSTREAM_DIR="${UPSTREAM_DIR:-$WORK_DIR/rockbox}"
PATCH_DIR="${PATCH_DIR:-$ROOT_DIR/patches}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out}"

ROCKBOX_REMOTE="${ROCKBOX_REMOTE:-https://github.com/Rockbox/rockbox.git}"
ROCKBOX_BASE_COMMIT="${ROCKBOX_BASE_COMMIT:-dd21a1d1d9ae4f314eb16d4813414ee72e2aa0de}"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-$ROOT_DIR/toolchain}"
RBDEV_DOWNLOAD="${RBDEV_DOWNLOAD:-$WORK_DIR/rbdev-download}"
RBDEV_BUILD="${RBDEV_BUILD:-$WORK_DIR/rbdev-build}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

mkdir -p "$WORK_DIR" "$OUT_DIR"

if [ ! -d "$UPSTREAM_DIR/.git" ]; then
  git clone "$ROCKBOX_REMOTE" "$UPSTREAM_DIR"
fi

git -C "$UPSTREAM_DIR" fetch --force origin "$ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" checkout --force "$ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" clean -fdx
git -C "$UPSTREAM_DIR" config user.name "rockbox-hiby-r1-ci"
git -C "$UPSTREAM_DIR" config user.email "ci@example.invalid"
(git -C "$UPSTREAM_DIR" am --abort >/dev/null 2>&1 || true)

mapfile -t PATCHES < <(find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' | sort)
if [ ${#PATCHES[@]} -eq 0 ]; then
  echo "No patches found in $PATCH_DIR" >&2
  exit 1
fi

for p in "${PATCHES[@]}"; do
  git -C "$UPSTREAM_DIR" am "$p"
done

PATCHED_HEAD="$(git -C "$UPSTREAM_DIR" rev-parse --verify HEAD)"

if [ ! -x "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" ]; then
  export RBDEV_PREFIX="$TOOLCHAIN_PREFIX"
  export RBDEV_DOWNLOAD
  export RBDEV_BUILD
  "$UPSTREAM_DIR/tools/rockboxdev.sh" --target=y
fi

export PATH="$TOOLCHAIN_PREFIX/bin:$PATH"

FW_BUILD_DIR="$WORK_DIR/build-hibyr1-rockbox"
BL_BUILD_DIR="$WORK_DIR/build-hibyr1-bootloader"

rm -rf "$FW_BUILD_DIR" "$BL_BUILD_DIR"
mkdir -p "$FW_BUILD_DIR" "$BL_BUILD_DIR"

(
  cd "$FW_BUILD_DIR"
  "$UPSTREAM_DIR/tools/configure" --target=hibyr1 --type=n
  make -j"$BUILD_JOBS"
)

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
META

(
  cd "$OUT_DIR"
  sha256sum $(ls -1) > SHA256SUMS
)

echo "Build complete. Artifacts in: $OUT_DIR"
ls -l "$OUT_DIR"
