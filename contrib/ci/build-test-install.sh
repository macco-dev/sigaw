#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=contrib/ci/common.sh
source "${SCRIPT_DIR}/common.sh"

repo_root="$(sigaw_repo_root)"
prefix="${SIGAW_PREFIX:-/usr}"
tmp_root="${RUNNER_TMP:-${RUNNER_TEMP:-/tmp}}"
build_dir="${SIGAW_BUILD_DIR:-${tmp_root}/sigaw-build}"
stage_dir="${SIGAW_STAGE_DIR:-${tmp_root}/sigaw-stage}"
skip_tests="${SIGAW_SKIP_TESTS:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="${2:-}"
            shift 2
            ;;
        --stage-dir)
            stage_dir="${2:-}"
            shift 2
            ;;
        --prefix)
            prefix="${2:-}"
            shift 2
            ;;
        --skip-tests)
            skip_tests="${2:-}"
            shift 2
            ;;
        *)
            sigaw_ci_fail "unknown argument: $1"
            ;;
    esac
done

sigaw_require_cmd meson ninja

sigaw_ci_log "preparing clean build directories"
rm -rf "${build_dir}" "${stage_dir}"

sigaw_ci_log "configuring Meson build in ${build_dir}"
meson setup "${build_dir}" "${repo_root}" \
    --prefix "${prefix}" \
    --buildtype release \
    --wrap-mode nodownload

sigaw_ci_log "compiling project"
meson compile -C "${build_dir}"

if [[ -z "${skip_tests}" ]]; then
    sigaw_ci_log "running full Meson test suite"
    meson test -C "${build_dir}" --no-rebuild --print-errorlogs
else
    sigaw_ci_log "running Meson tests with skip list: ${skip_tests}"
    declare -A skip_set=()
    IFS=',' read -r -a skip_items <<< "${skip_tests}"
    for item in "${skip_items[@]}"; do
        if [[ -n "${item}" ]]; then
            skip_set["${item}"]=1
        fi
    done

    mapfile -t all_tests < <(meson test -C "${build_dir}" --list)
    selected_tests=()
    for test_name in "${all_tests[@]}"; do
        if [[ -z "${skip_set[${test_name}]+x}" ]]; then
            selected_tests+=("${test_name}")
        fi
    done

    if [[ "${#selected_tests[@]}" -eq 0 ]]; then
        sigaw_ci_fail "skip list removed every test"
    fi

    meson test -C "${build_dir}" --no-rebuild --print-errorlogs "${selected_tests[@]}"
fi

sigaw_ci_log "installing into staged DESTDIR ${stage_dir}"
DESTDIR="${stage_dir}" meson install -C "${build_dir}" --no-rebuild

install_root="${stage_dir}${prefix}"
expected_paths=(
    "bin/sigaw-daemon"
    "bin/sigaw-ctl"
    "bin/sigaw-run"
    "lib/libSigaw.so"
    "lib/libSigawGL.so"
    "share/sigaw/shaders/overlay_quad.vert.spv"
    "share/sigaw/shaders/overlay_quad.frag.spv"
    "share/vulkan/implicit_layer.d/VK_LAYER_MACCO_sigaw.x86_64.json"
    "share/systemd/user/sigaw-daemon.service"
    "share/doc/sigaw/sigaw.conf.example"
    "share/sigaw/fonts/NotoSans-Medium.ttf"
    "share/sigaw/fonts/NotoSans-Bold.ttf"
    "share/sigaw/fonts/NotoSans-LICENSE"
    "share/sigaw/icons/sigaw-logo.svg"
)

sigaw_ci_log "verifying staged install contents"
for relative_path in "${expected_paths[@]}"; do
    if [[ ! -f "${install_root}/${relative_path}" ]]; then
        sigaw_ci_fail "missing staged install file: ${install_root}/${relative_path}"
    fi
done

if [[ -n "${FORGEJO_OUTPUT:-}" ]]; then
    {
        printf 'build_dir=%s\n' "${build_dir}"
        printf 'stage_dir=%s\n' "${stage_dir}"
        printf 'install_root=%s\n' "${install_root}"
    } >> "${FORGEJO_OUTPUT}"
fi

sigaw_summary "### Sigaw CI build"
sigaw_summary "- Build directory: \`${build_dir}\`"
sigaw_summary "- Stage directory: \`${stage_dir}\`"
sigaw_summary "- Install root: \`${install_root}\`"

sigaw_ci_log "build, test, and staged install smoke completed"
