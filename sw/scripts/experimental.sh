#!/usr/bin/env bash
# Combine the experimental feature branches with the source being built.
#
# Experimental features live on their own git branches, listed in
# sw/experimental/branches.txt. This script merges all of them on top of the
# current commit, so that a build of the result carries every experimental
# feature at once. See sw/experimental/README.md.
#
#   experimental.sh list                 print the feature branches
#   experimental.sh merge --worktree DIR make DIR a checkout of HEAD with the
#                                        features merged in (the tree you are
#                                        working in is left alone)
#   experimental.sh merge --in-place     merge the features into the current
#                                        checkout (for throwaway checkouts,
#                                        such as CI)
#   experimental.sh describe [DIR]       describe the experimental features
#                                        present in a source tree
#   experimental.sh source [DIR]         the commit a source tree is, with the
#                                        feature branches "merge" added to it

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${SW_DIR}/.." && pwd)"
BRANCH_LIST="${SW_DIR}/experimental/branches.txt"

usage() {
    sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
    cat <<EOF
Options for merge:
  -r, --remote NAME     remote to take branches from when there is no local
                        branch of that name (default: origin)
  -f, --fetch           fetch the branches from the remote first
EOF
}

# Branch names from the list, without comments and blank lines
feature_branches() {
    [[ -f "${BRANCH_LIST}" ]] || return 0
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^[[:space:]]*$/d' "${BRANCH_LIST}"
}

# The commit to merge for a feature branch: a local branch if there is one
# (the one being worked on), else the remote's
resolve_branch() {
    local branch="$1" remote="$2"
    if git -C "${REPO_DIR}" rev-parse --verify --quiet "refs/heads/${branch}^{commit}" >/dev/null; then
        echo "refs/heads/${branch}"
    elif git -C "${REPO_DIR}" rev-parse --verify --quiet "refs/remotes/${remote}/${branch}^{commit}" >/dev/null; then
        echo "refs/remotes/${remote}/${branch}"
    else
        return 1
    fi
}

# Give a new worktree the submodules of the main checkout, cloned locally
# rather than downloaded again
populate_submodules() {
    local tree="$1" name path
    git -C "${tree}" submodule init --quiet
    while read -r name path; do
        if [[ -e "${REPO_DIR}/${path}/.git" ]]; then
            git -C "${tree}" config "submodule.${name}.url" "${REPO_DIR}/${path}"
        fi
    done < <(git -C "${tree}" config --file .gitmodules --get-regexp '^submodule\..*\.path$' |
             sed -e 's/^submodule\.\(.*\)\.path /\1 /')
    git -C "${tree}" -c protocol.file.allow=always submodule update --quiet
}

# Subject of the merge commits "merge" makes, which "source" recognises
MERGE_SUBJECT="Experimental build: merge "

cmd_merge() {
    local tree="" remote="origin" fetch=0 in_place=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --worktree) tree="$2"; shift 2 ;;
            --in-place) in_place=1; shift ;;
            -r|--remote) remote="$2"; shift 2 ;;
            -f|--fetch) fetch=1; shift ;;
            *) echo "Unknown option '$1'" >&2; usage >&2; exit 2 ;;
        esac
    done
    if [[ ${in_place} -eq 1 && -n "${tree}" ]] || [[ ${in_place} -eq 0 && -z "${tree}" ]]; then
        echo "merge needs either --worktree DIR or --in-place" >&2
        exit 2
    fi

    local branches
    mapfile -t branches < <(feature_branches)
    if [[ ${#branches[@]} -eq 0 ]]; then
        echo "No experimental feature branches are listed in ${BRANCH_LIST}" >&2
        exit 1
    fi

    if [[ ${fetch} -eq 1 ]]; then
        git -C "${REPO_DIR}" fetch --quiet "${remote}" "${branches[@]}"
    fi

    local refs=() branch ref
    for branch in "${branches[@]}"; do
        if ! ref="$(resolve_branch "${branch}" "${remote}")"; then
            echo "Experimental feature branch '${branch}' was not found, locally or on '${remote}'." >&2
            echo "Fetch it (experimental.sh merge --fetch ...), or take it out of ${BRANCH_LIST}." >&2
            exit 1
        fi
        refs+=("${ref}")
    done

    if [[ ${in_place} -eq 1 ]]; then
        tree="${REPO_DIR}"
        if ! git -C "${tree}" diff --quiet HEAD --; then
            echo "The checkout has uncommitted changes; commit them or use --worktree DIR." >&2
            exit 1
        fi
    else
        if ! git -C "${REPO_DIR}" diff --quiet HEAD --; then
            echo "Note: uncommitted changes in ${REPO_DIR} are not part of the experimental build." >&2
        fi
        if [[ -e "${tree}" ]]; then
            git -C "${REPO_DIR}" worktree remove --force "${tree}" 2>/dev/null || rm -rf "${tree}"
        fi
        git -C "${REPO_DIR}" worktree prune
        git -C "${REPO_DIR}" worktree add --quiet --detach "${tree}" HEAD
        populate_submodules "${tree}"
    fi

    local i
    for i in "${!branches[@]}"; do
        branch="${branches[$i]}"
        ref="${refs[$i]}"
        echo "Merging experimental feature ${branch} ($(git -C "${tree}" rev-parse --short "${ref}"))"
        if ! git -C "${tree}" -c user.name="PicoGUS build" -c user.email="picogus-build@localhost" \
                merge --quiet --no-ff --no-edit -m "${MERGE_SUBJECT}${branch}" "${ref}"; then
            echo >&2
            echo "'${branch}' does not merge cleanly here. Conflicting files:" >&2
            git -C "${tree}" diff --name-only --diff-filter=U >&2 || true
            git -C "${tree}" merge --abort || true
            echo "Bring the feature branch up to date (merge this branch into it, or rebase it) and resolve the conflict there." >&2
            exit 1
        fi
    done
    # In case a feature moves a submodule to another commit
    git -C "${tree}" -c protocol.file.allow=always submodule update --quiet
    echo "Experimental source tree: ${tree}"
    cmd_describe "${tree}"
}

# Each feature describes itself in sw/experimental/<name>.md; the first line
# is its title
cmd_describe() {
    local tree="${1:-${REPO_DIR}}" file found=0
    for file in "${tree}"/sw/experimental/*.md; do
        [[ -f "${file}" && "$(basename "${file}")" != README.md ]] || continue
        found=1
        printf '  %-14s %s\n' "$(basename "${file}" .md)" "$(head -n 1 "${file}" | sed -e 's/^#* *//')"
    done
    [[ ${found} -eq 1 ]] || echo "  (no experimental features in this source tree)"
}

# "<commit> + <branch>@<commit> + ...": the commit the tree was made from and
# the feature branches merged on top of it by "merge"
cmd_source() {
    local tree="${1:-${REPO_DIR}}" rev=HEAD subject merged=""
    while subject="$(git -C "${tree}" log -1 --format=%s "${rev}" 2>/dev/null)" &&
          [[ "${subject}" == "${MERGE_SUBJECT}"* ]]; do
        merged=" + ${subject#"${MERGE_SUBJECT}"}@$(git -C "${tree}" rev-parse --short "${rev}^2")${merged}"
        rev="${rev}^"
    done
    echo "$(git -C "${tree}" describe --always --tags "${rev}" 2>/dev/null || echo unknown)${merged}"
}

case "${1:-}" in
    list) feature_branches ;;
    merge) shift; cmd_merge "$@" ;;
    describe) shift; cmd_describe "$@" ;;
    source) shift; cmd_source "$@" ;;
    -h|--help|help) usage ;;
    *) usage >&2; exit 2 ;;
esac
