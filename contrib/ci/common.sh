#!/usr/bin/env bash

set -euo pipefail

sigaw_ci_log() {
    printf '[sigaw-ci] %s\n' "$*"
}

sigaw_ci_fail() {
    printf '[sigaw-ci] ERROR: %s\n' "$*" >&2
    exit 1
}

sigaw_repo_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "${script_dir}/../.." && pwd
}

sigaw_require_cmd() {
    local cmd
    for cmd in "$@"; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            sigaw_ci_fail "required command not found: ${cmd}"
        fi
    done
}

sigaw_version_from_meson() {
    local repo_root version
    repo_root="${1:-$(sigaw_repo_root)}"
    version="$(awk -F"'" '/version:[[:space:]]*'\''/ { print $2; exit }' "${repo_root}/meson.build")"
    if [[ -z "${version}" ]]; then
        sigaw_ci_fail "could not determine project version from meson.build"
    fi
    printf '%s\n' "${version}"
}

sigaw_truthy() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

sigaw_repo_slug() {
    if [[ -n "${FORGEJO_REPOSITORY:-}" ]]; then
        printf '%s\n' "${FORGEJO_REPOSITORY}"
        return 0
    fi
    if [[ -n "${GITHUB_REPOSITORY:-}" ]]; then
        printf '%s\n' "${GITHUB_REPOSITORY}"
        return 0
    fi

    local remote
    remote="$(git config --get remote.origin.url 2>/dev/null || true)"
    if [[ "${remote}" =~ ^https?://[^/]+/([^/]+/[^/]+)(\.git)?$ ]]; then
        printf '%s\n' "${BASH_REMATCH[1]%.git}"
        return 0
    fi
    if [[ "${remote}" =~ ^git@[^:]+:([^/]+/[^/]+)(\.git)?$ ]]; then
        printf '%s\n' "${BASH_REMATCH[1]%.git}"
        return 0
    fi

    sigaw_ci_fail "could not determine repository slug"
}

sigaw_server_url() {
    if [[ -n "${FORGEJO_SERVER_URL:-}" ]]; then
        printf '%s\n' "${FORGEJO_SERVER_URL%/}"
        return 0
    fi
    if [[ -n "${GITHUB_SERVER_URL:-}" ]]; then
        printf '%s\n' "${GITHUB_SERVER_URL%/}"
        return 0
    fi

    local remote
    remote="$(git config --get remote.origin.url 2>/dev/null || true)"
    if [[ "${remote}" =~ ^(https?://[^/]+)/[^/]+/[^/]+(\.git)?$ ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
        return 0
    fi

    sigaw_ci_fail "could not determine Forgejo server URL"
}

sigaw_api_url() {
    if [[ -n "${FORGEJO_API_URL:-}" ]]; then
        printf '%s\n' "${FORGEJO_API_URL%/}"
        return 0
    fi
    if [[ -n "${GITHUB_API_URL:-}" ]]; then
        printf '%s\n' "${GITHUB_API_URL%/}"
        return 0
    fi

    printf '%s/api/v1\n' "$(sigaw_server_url)"
}

sigaw_release_token() {
    if [[ -n "${FORGEJO_RELEASE_TOKEN:-}" ]]; then
        printf '%s\n' "${FORGEJO_RELEASE_TOKEN}"
        return 0
    fi
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        printf '%s\n' "${GITHUB_TOKEN}"
        return 0
    fi
    if [[ -n "${FORGEJO_TOKEN:-}" ]]; then
        printf '%s\n' "${FORGEJO_TOKEN}"
        return 0
    fi

    sigaw_ci_fail "set FORGEJO_RELEASE_TOKEN, GITHUB_TOKEN, or FORGEJO_TOKEN"
}

sigaw_current_sha() {
    if [[ -n "${FORGEJO_SHA:-}" ]]; then
        printf '%s\n' "${FORGEJO_SHA}"
        return 0
    fi
    if [[ -n "${GITHUB_SHA:-}" ]]; then
        printf '%s\n' "${GITHUB_SHA}"
        return 0
    fi

    git rev-parse HEAD
}

sigaw_current_ref_name() {
    if [[ -n "${FORGEJO_REF_NAME:-}" ]]; then
        printf '%s\n' "${FORGEJO_REF_NAME}"
        return 0
    fi
    if [[ -n "${GITHUB_REF_NAME:-}" ]]; then
        printf '%s\n' "${GITHUB_REF_NAME}"
        return 0
    fi

    git rev-parse --abbrev-ref HEAD
}

sigaw_summary() {
    local line
    for line in "$@"; do
        if [[ -n "${FORGEJO_STEP_SUMMARY:-}" ]]; then
            printf '%s\n' "${line}" >> "${FORGEJO_STEP_SUMMARY}"
        elif [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
            printf '%s\n' "${line}" >> "${GITHUB_STEP_SUMMARY}"
        fi
    done
}
