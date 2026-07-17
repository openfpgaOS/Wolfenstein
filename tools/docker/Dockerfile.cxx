# openfpgaOS — C++-capable RISC-V build container (xPack toolchain).
#
# ECWolf throws and catches C++ exceptions, so app.elf must link the
# toolchain's REAL libsupc++ (the EH runtime: __cxa_throw, the personality
# routine, …).  The shared C-only firmware image (tools/docker/
# Dockerfile.firmware) cannot provide it: Ubuntu's gcc-riscv64-unknown-elf
# (and Homebrew's riscv64-elf-gcc) ship no bare-metal C++ runtime at all —
# no rv32 libsupc++.a exists to link.  Same story as the Diablo repo's
# tools/docker/Dockerfile.diablo, which this image mirrors.
#
# This image carries the xPack riscv-none-elf-gcc toolchain, a freely
# redistributable bare-metal RISC-V GCC that bundles newlib + a full
# multilib set of libstdc++/libsupc++ (including rv32imafc / ilp32f).
# src/wolfenstein/Makefile points the shared sdk-container.sh wrapper here
# via SDK_IMG/SDK_DOCKERFILE and pre-sets CROSS=riscv-none-elf- so sdk.mk
# uses the xPack prefix inside the container.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Pin the xPack toolchain release. Bump this to adopt a newer GCC; the asset
# URL scheme (xpack-riscv-none-elf-gcc-<ver>-linux-<arch>.tar.gz) is stable.
ARG XPACK_VER=15.2.0-1
# Provided automatically by BuildKit (arm64 on Apple silicon, amd64 on x86).
ARG TARGETARCH

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        make \
        python3 \
        curl \
        xz-utils \
        bsdmainutils \
        ca-certificates \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# Fetch + unpack the xPack toolchain matching the image architecture. The
# tarball's top level is a single versioned dir; --strip-components=1 lands the
# bin/ lib/ etc. directly under /opt/xpack so /opt/xpack/bin is on PATH.
RUN set -eux; \
    case "${TARGETARCH}" in \
      arm64) XARCH=linux-arm64 ;; \
      amd64) XARCH=linux-x64   ;; \
      *) echo "unsupported TARGETARCH='${TARGETARCH}'" >&2; exit 1 ;; \
    esac; \
    url="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${XPACK_VER}/xpack-riscv-none-elf-gcc-${XPACK_VER}-${XARCH}.tar.gz"; \
    mkdir -p /opt/xpack; \
    curl -fsSL "$url" | tar -xz -C /opt/xpack --strip-components=1; \
    /opt/xpack/bin/riscv-none-elf-g++ --version

ENV PATH=/opt/xpack/bin:$PATH
