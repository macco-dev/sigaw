#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=contrib/ci/common.sh
source "${SCRIPT_DIR}/common.sh"

repo_root="$(sigaw_repo_root)"
tmp_root="${RUNNER_TMP:-${RUNNER_TEMP:-/tmp}}"
build_dir="${SIGAW_BUILD_DIR:-${tmp_root}/sigaw-build}"
stage_dir="${SIGAW_STAGE_DIR:-${tmp_root}/sigaw-stage}"
output_dir="${SIGAW_DIST_DIR:-${tmp_root}/sigaw-dist}"
version_label="${SIGAW_VERSION_LABEL:-$(sigaw_version_from_meson "${repo_root}")}"
arch="${SIGAW_ARCHIVE_ARCH:-$(uname -m)}"
ref_name="$(sigaw_current_ref_name)"
commit_sha="$(sigaw_current_sha)"
timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

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
        --output-dir)
            output_dir="${2:-}"
            shift 2
            ;;
        --version-label)
            version_label="${2:-}"
            shift 2
            ;;
        --arch)
            arch="${2:-}"
            shift 2
            ;;
        *)
            sigaw_ci_fail "unknown argument: $1"
            ;;
    esac
done

sigaw_require_cmd tar xz sha256sum

if [[ ! -x "${build_dir}/sigaw-render-screenshots" ]]; then
    sigaw_ci_fail "expected screenshot renderer at ${build_dir}/sigaw-render-screenshots"
fi
if [[ ! -d "${stage_dir}" ]]; then
    sigaw_ci_fail "expected staged install directory at ${stage_dir}"
fi

mkdir -p "${output_dir}"
rm -f "${output_dir}/sigaw-${version_label}-"*

scratch_dir="$(mktemp -d "${tmp_root%/}/sigaw-package-XXXXXX")"
trap 'rm -rf "${scratch_dir}"' EXIT

generated_screenshots_dir="${scratch_dir}/screenshots"
mkdir -p "${generated_screenshots_dir}"

sigaw_ci_log "rendering screenshots into ${generated_screenshots_dir}"
"${build_dir}/sigaw-render-screenshots" --output-dir "${generated_screenshots_dir}"

archive_prefix="sigaw-${version_label}"
install_archive="${output_dir}/${archive_prefix}-linux-${arch}.tar.xz"
screenshots_archive="${output_dir}/${archive_prefix}-screenshots.tar.xz"
metadata_file="${output_dir}/${archive_prefix}-build-metadata.txt"
checksums_file="${output_dir}/${archive_prefix}-SHA256SUMS.txt"

sigaw_ci_log "creating staged install archive ${install_archive}"
tar -C "${stage_dir}" -cJf "${install_archive}" .

sigaw_ci_log "creating screenshots archive ${screenshots_archive}"
tar -C "${scratch_dir}" -cJf "${screenshots_archive}" screenshots

cat > "${metadata_file}" <<EOF
version_label=${version_label}
ref_name=${ref_name}
commit_sha=${commit_sha}
build_timestamp=${timestamp}
archive_arch=${arch}
repository=$(sigaw_repo_slug)
EOF

(
    cd "${output_dir}"
    sha256sum \
        "$(basename "${install_archive}")" \
        "$(basename "${screenshots_archive}")" \
        "$(basename "${metadata_file}")" \
        > "$(basename "${checksums_file}")"
)

if [[ -n "${FORGEJO_OUTPUT:-}" ]]; then
    {
        printf 'dist_dir=%s\n' "${output_dir}"
        printf 'install_archive=%s\n' "${install_archive}"
        printf 'screenshots_archive=%s\n' "${screenshots_archive}"
        printf 'metadata_file=%s\n' "${metadata_file}"
        printf 'checksums_file=%s\n' "${checksums_file}"
    } >> "${FORGEJO_OUTPUT}"
fi

sigaw_summary "### Sigaw packaged artifacts"
sigaw_summary "- Install archive: \`$(basename "${install_archive}")\`"
sigaw_summary "- Screenshots archive: \`$(basename "${screenshots_archive}")\`"
sigaw_summary "- Checksums: \`$(basename "${checksums_file}")\`"

sigaw_ci_log "packaged release assets into ${output_dir}"
