#!/usr/bin/env bash
# Regenerate nix/submodules.json from the checked-out submodule commits.
#
# The Nix package fetches only the submodules a Linux build needs, pinned to the
# commits recorded in the superproject. Run this after syncing with upstream or
# bumping any submodule.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Each entry is "path" or "path:shallow-only". shallow-only fetches the repository
# without its own submodules (build-deps vendors ~1.5 GiB of FFmpeg sources we never use;
# CMake only needs its package-lock.cmake).
paths=(
  third-party/Simple-Web-Server
  third-party/build-deps:shallow-only
  third-party/glad
  third-party/libdisplaydevice
  third-party/libvirtualhid
  third-party/lizardbyte-common
  third-party/moonlight-common-c
  third-party/plasma-wayland-protocols
  third-party/tray
  third-party/wayland-protocols
  third-party/wlr-protocols
)

{
  echo "["
  sep=""
  for entry in "${paths[@]}"; do
    path="${entry%%:*}"
    submodules=true
    if [[ "${entry}" == *:shallow-only ]]; then
      submodules=false
    fi
    url="$(git config --file .gitmodules --get "submodule.${path}.url")"
    rev="$(git ls-tree HEAD "${path}" | awk '{ print $3 }')"
    if [[ -z "${url}" || -z "${rev}" ]]; then
      echo "missing submodule metadata for ${path}" >&2
      exit 1
    fi
    printf '%s  { "path": "%s", "url": "%s", "rev": "%s", "submodules": %s }' "${sep}" "${path}" "${url}" "${rev}" "${submodules}"
    sep=$',\n'
  done
  printf '\n]\n'
} > nix/submodules.json

echo "wrote nix/submodules.json"
