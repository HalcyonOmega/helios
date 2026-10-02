#!/usr/bin/env bash
# Incremental developer build inside the flake dev shell.
#
#   nix/dev-build.sh [build-dir]
#
# Uses the same feature flags as nix/package.nix, but builds from the local checkout
# (submodules must be initialized) into an incremental Ninja tree. The binary lands
# at <build-dir>/sunshine and runs from the build tree.
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
build_dir="${1:-${XDG_CACHE_HOME:-$HOME/.cache}/stream-host/build}"
ffmpeg_link="$(dirname "${build_dir}")/ffmpeg"

if [[ ! -e "${ffmpeg_link}" ]]; then
  nix build "${root}#default.ffmpeg" --out-link "${ffmpeg_link}"
fi

if [[ ! -d "${root}/node_modules" ]]; then
  (cd "${root}" && nix develop "${root}" --command npm ci --ignore-scripts --no-audit --no-fund)
fi

if [[ ! -f "${build_dir}/build.ninja" ]]; then
  nix develop "${root}" --command cmake -S "${root}" -B "${build_dir}" -G Ninja \
    -Wno-dev \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=OFF \
    -DBOOST_USE_STATIC=OFF \
    -DNPM_SKIP_INSTALL=ON \
    -DGLAD_SKIP_PIP_INSTALL=ON \
    -DSUNSHINE_SYSTEM_VULKAN_HEADERS=ON \
    -DSUNSHINE_ENABLE_CUDA=OFF \
    -DCUDA_FAIL_ON_MISSING=OFF \
    -DFFMPEG_PREPARED_BINARIES="$(readlink -f "${ffmpeg_link}")/ffmpeg" \
    -DUDEV_RULES_INSTALL_DIR=lib/udev/rules.d \
    -DSYSTEMD_USER_UNIT_INSTALL_DIR=lib/systemd/user \
    -DSYSTEMD_SYSTEM_UNIT_INSTALL_DIR=lib/systemd/system \
    -DSYSTEMD_MODULES_LOAD_DIR=lib/modules-load.d
fi

nix develop "${root}" --command cmake --build "${build_dir}" --target sunshine
