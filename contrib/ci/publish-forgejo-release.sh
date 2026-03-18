#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=contrib/ci/common.sh
source "${SCRIPT_DIR}/common.sh"

sigaw_require_cmd jq curl

tag_name=""
release_name=""
target_commitish=""
body_file=""
asset_dir=""
prerelease="false"
draft="false"
dry_run="false"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag)
            tag_name="${2:-}"
            shift 2
            ;;
        --title|--name)
            release_name="${2:-}"
            shift 2
            ;;
        --target)
            target_commitish="${2:-}"
            shift 2
            ;;
        --body-file)
            body_file="${2:-}"
            shift 2
            ;;
        --asset-dir)
            asset_dir="${2:-}"
            shift 2
            ;;
        --prerelease)
            prerelease="true"
            shift
            ;;
        --draft)
            draft="true"
            shift
            ;;
        --dry-run)
            dry_run="true"
            shift
            ;;
        *)
            sigaw_ci_fail "unknown argument: $1"
            ;;
    esac
done

if [[ -z "${tag_name}" || -z "${release_name}" || -z "${target_commitish}" || -z "${body_file}" || -z "${asset_dir}" ]]; then
    sigaw_ci_fail "usage: publish-forgejo-release.sh --tag <tag> --title <title> --target <sha> --body-file <file> --asset-dir <dir> [--prerelease] [--draft] [--dry-run]"
fi
if [[ ! -f "${body_file}" ]]; then
    sigaw_ci_fail "missing release body file: ${body_file}"
fi
if [[ ! -d "${asset_dir}" ]]; then
    sigaw_ci_fail "missing asset directory: ${asset_dir}"
fi

mapfile -t assets < <(find "${asset_dir}" -maxdepth 1 -type f | sort)
if [[ "${#assets[@]}" -eq 0 ]]; then
    sigaw_ci_fail "no assets found in ${asset_dir}"
fi

release_body="$(<"${body_file}")"
repo_api="$(sigaw_api_url)/repos/$(sigaw_repo_slug)"
release_payload="$(jq -n \
    --arg tag_name "${tag_name}" \
    --arg target_commitish "${target_commitish}" \
    --arg name "${release_name}" \
    --arg body "${release_body}" \
    --argjson draft "${draft}" \
    --argjson prerelease "${prerelease}" \
    '{
        tag_name: $tag_name,
        target_commitish: $target_commitish,
        name: $name,
        body: $body,
        draft: $draft,
        prerelease: $prerelease
    }')"

if sigaw_truthy "${dry_run}"; then
    sigaw_ci_log "dry-run mode enabled"
    sigaw_ci_log "would upsert release ${tag_name} against ${repo_api}"
    for asset in "${assets[@]}"; do
        sigaw_ci_log "would upload asset $(basename "${asset}")"
    done
    exit 0
fi

token="$(sigaw_release_token)"

api_request() {
    local method="$1"
    local url="$2"
    local payload="${3:-}"
    local output_file="$4"

    if [[ -n "${payload}" ]]; then
        curl -sS -o "${output_file}" -w '%{http_code}' \
            -X "${method}" \
            -H "Authorization: token ${token}" \
            -H 'Content-Type: application/json' \
            --data "${payload}" \
            "${url}"
    else
        curl -sS -o "${output_file}" -w '%{http_code}' \
            -X "${method}" \
            -H "Authorization: token ${token}" \
            "${url}"
    fi
}

release_lookup="$(mktemp)"
trap 'rm -f "${release_lookup}" "${release_response:-}" "${assets_response:-}"' EXIT
http_code="$(api_request GET "${repo_api}/releases/tags/${tag_name}" "" "${release_lookup}")"

release_response="$(mktemp)"
if [[ "${http_code}" == "200" ]]; then
    release_id="$(jq -r '.id' "${release_lookup}")"
    sigaw_ci_log "updating existing release ${tag_name} (id ${release_id})"
    http_code="$(api_request PATCH "${repo_api}/releases/${release_id}" "${release_payload}" "${release_response}")"
    if [[ "${http_code}" != "200" ]]; then
        cat "${release_response}" >&2
        sigaw_ci_fail "failed to update release ${tag_name} (HTTP ${http_code})"
    fi
elif [[ "${http_code}" == "404" ]]; then
    sigaw_ci_log "creating release ${tag_name}"
    http_code="$(api_request POST "${repo_api}/releases" "${release_payload}" "${release_response}")"
    if [[ "${http_code}" != "201" ]]; then
        cat "${release_response}" >&2
        sigaw_ci_fail "failed to create release ${tag_name} (HTTP ${http_code})"
    fi
else
    cat "${release_lookup}" >&2
    sigaw_ci_fail "failed to query release ${tag_name} (HTTP ${http_code})"
fi

release_id="$(jq -r '.id' "${release_response}")"
release_url="$(jq -r '.html_url // empty' "${release_response}")"

assets_response="$(mktemp)"
http_code="$(api_request GET "${repo_api}/releases/${release_id}/assets" "" "${assets_response}")"
if [[ "${http_code}" != "200" ]]; then
    cat "${assets_response}" >&2
    sigaw_ci_fail "failed to query existing release assets (HTTP ${http_code})"
fi

for asset in "${assets[@]}"; do
    asset_name="$(basename "${asset}")"
    asset_id="$(jq -r --arg name "${asset_name}" '.[] | select(.name == $name) | .id' "${assets_response}" | head -n1)"
    if [[ -n "${asset_id}" ]]; then
        delete_response="$(mktemp)"
        http_code="$(api_request DELETE "${repo_api}/releases/${release_id}/assets/${asset_id}" "" "${delete_response}")"
        rm -f "${delete_response}"
        if [[ "${http_code}" != "204" ]]; then
            sigaw_ci_fail "failed to delete existing asset ${asset_name} (HTTP ${http_code})"
        fi
    fi

    sigaw_ci_log "uploading asset ${asset_name}"
    encoded_name="$(printf '%s' "${asset_name}" | jq -sRr @uri)"
    upload_response="$(mktemp)"
    http_code="$(curl -sS -o "${upload_response}" -w '%{http_code}' \
        -X POST \
        -H "Authorization: token ${token}" \
        -H 'Content-Type: application/octet-stream' \
        --data-binary "@${asset}" \
        "${repo_api}/releases/${release_id}/assets?name=${encoded_name}")"
    rm -f "${upload_response}"
    if [[ "${http_code}" != "201" ]]; then
        sigaw_ci_fail "failed to upload asset ${asset_name} (HTTP ${http_code})"
    fi
done

if [[ -n "${FORGEJO_OUTPUT:-}" ]]; then
    {
        printf 'release_id=%s\n' "${release_id}"
        printf 'release_url=%s\n' "${release_url}"
    } >> "${FORGEJO_OUTPUT}"
fi

sigaw_summary "### Forgejo release"
sigaw_summary "- Tag: \`${tag_name}\`"
sigaw_summary "- Assets: ${#assets[@]}"
if [[ -n "${release_url}" ]]; then
    sigaw_summary "- URL: ${release_url}"
fi

sigaw_ci_log "release ${tag_name} published"
