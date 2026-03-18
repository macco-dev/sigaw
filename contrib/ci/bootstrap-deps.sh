#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=contrib/ci/common.sh
source "${SCRIPT_DIR}/common.sh"

mode="install"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            mode="${2:-}"
            shift 2
            ;;
        --check)
            mode="check"
            shift
            ;;
        *)
            sigaw_ci_fail "unknown argument: $1"
            ;;
    esac
done

case "${mode}" in
    install|check)
        ;;
    *)
        sigaw_ci_fail "unsupported mode: ${mode}"
        ;;
esac

if [[ "${mode}" == "install" ]]; then
    if ! command -v apt-get >/dev/null 2>&1; then
        sigaw_ci_fail "apt-get is required for install mode"
    fi

    apt_cmd=(apt-get)
    if command -v sudo >/dev/null 2>&1 && [[ "$(id -u)" -ne 0 ]]; then
        apt_cmd=(sudo "${apt_cmd[@]}")
    fi

    packages=(
        build-essential
        pkg-config
        meson
        ninja-build
        glslang-tools
        jq
        git
        curl
        xz-utils
        libvulkan-dev
        libgl-dev
        libegl-dev
        libx11-dev
        nlohmann-json3-dev
        libcurl4-openssl-dev
        libfreetype-dev
        libpng-dev
        libgtk-3-dev
        libayatana-appindicator3-dev
    )

    sigaw_ci_log "installing CI dependencies with apt"
    "${apt_cmd[@]}" update
    "${apt_cmd[@]}" install -y --no-install-recommends "${packages[@]}"
fi

sigaw_require_cmd cc c++ meson ninja pkg-config glslangValidator jq git curl sha256sum tar xz
sigaw_ci_log "toolchain dependencies are available"
