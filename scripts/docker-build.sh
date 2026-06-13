#!/usr/bin/env bash
# Reproducible local build of the HiBy R1 Rockbox firmware inside a Linux
# container (matches the ubuntu-22.04 CI runner). Runs scripts/build.sh.
#
#   scripts/docker-build.sh            # full firmware + bootloader build
#   scripts/docker-build.sh <cmd...>   # run an arbitrary command in the env
#
# Why the Docker volumes: the cross-toolchain includes glibc, whose build
# relies on filenames that differ only by case (stamp.os vs stamp.oS). The
# macOS bind mount is case-INsensitive (APFS), so glibc fails there with
# "mv: '...oST' and '...oS' are the same file". We therefore put the toolchain
# build/install on case-sensitive Docker volumes (ext4 in the Linux VM), which
# also persist the built toolchain between runs. Only the repo source
# (overlay/, patches/, scripts/) and the final artifacts (out/) use the bind
# mount, so builds land back on the host.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${IMG:-rockbox-hiby-build}"
TC_VOL="${TC_VOL:-rockbox-hiby-toolchain}"   # case-sensitive, persistent toolchain
WORK_VOL="${WORK_VOL:-rockbox-hiby-work}"     # case-sensitive build scratch + src

docker build -t "$IMG" "$ROOT_DIR/docker"

if [ "$#" -eq 0 ]; then
  set -- bash scripts/build.sh
fi

exec docker run --rm \
  -e GNU_MIRROR -e LINUX_MIRROR \
  -e R1_UPDATE_URL -e R1_UPDATE_SHA256 -e R1_UPDATE_PATH \
  -e TOOLCHAIN_PREFIX=/work/toolchain/tc \
  -v "$ROOT_DIR":/work \
  -v "$TC_VOL":/work/toolchain \
  -v "$WORK_VOL":/work/work \
  -w /work \
  "$IMG" \
  bash -c 'git config --global --add safe.directory "*"; exec "$@"' _ "$@"
