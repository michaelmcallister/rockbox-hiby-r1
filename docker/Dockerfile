# Build environment for HiBy R1 Rockbox, matching the ubuntu-22.04 CI runner.
# Used by scripts/docker-build.sh to produce rockbox.r1 / rockbox.zip on a Mac
# (or anywhere with Docker), since the MIPS cross-toolchain builds cleanly on
# Linux but not natively on macOS.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git xz-utils bzip2 gzip zip patch texinfo \
        automake libtool libtool-bin autoconf flex bison gawk \
        gtk-doc-tools pkg-config gettext ccache \
        libgmp-dev libmpfr-dev libmpc-dev libisl-dev \
        rsync ca-certificates curl wget \
        p7zip-full squashfs-tools genisoimage \
    && rm -rf /var/lib/apt/lists/*
