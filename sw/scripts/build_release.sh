#!/usr/bin/env bash
# Build the PicoGUS release files: the firmware images for RP2040 (Pico, Pico W,
# PicoGUS 2.0), RP2350 (Pico 2, Pico 2 W) and the RP2350B PicoGUS v2.1 THT
# board, and the DOS tools.
#
#   build_release.sh                  mainline release files in binaries/
#   build_release.sh --experimental   the same, plus binaries/experimental/
#                                     with a second, complete set built with
#                                     all experimental features merged in
#
# The mainline set is built from the source tree this script is in. For the
# experimental set, sw/scripts/experimental.sh merges the feature branches
# listed in sw/experimental/branches.txt into a separate checkout of the
# current commit (build/experimental-src), which is then built the same way.
#
# Firmware needs PICO_SDK_PATH, PICO_EXTRAS_PATH and arm-none-eabi-gcc; the DOS
# tools need OpenWatcom (wcl on PATH and WATCOM set) and are skipped without it.
#
# CI runs this script once per flavor, chip and kind of file, in a checkout
# that already has the experimental features merged in for the experimental
# flavor (see .github/workflows/build.yml).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${SW_DIR}/.." && pwd)"

usage() {
    sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
    cat <<EOF
Options:
  -e, --experimental    also build the experimental set
      --only-experimental  build only the experimental set
  -p, --platform NAME   rp2040, rp2350, picogus21 (RP2350B PicoGUS v2.1) or all (default: all)
  -o, --out DIR         where to collect the files (default: ${REPO_DIR}/binaries)
  -b, --build-dir DIR   parent of the build directories (default: ${REPO_DIR}/build)
  -j, --jobs N          parallel build jobs (default: number of CPUs)
  -c, --clean           delete each build directory before configuring
      --no-firmware     skip the firmware images
      --no-dos-tools    skip the DOS tools and the documentation
      --require-dos-tools  fail instead of skipping when OpenWatcom is missing
  -r, --remote NAME     remote to take the experimental branches from when
                        there is no local branch of that name (default: origin)
  -f, --fetch           fetch the experimental branches from the remote first
  -h, --help            show this help
EOF
}

MAINLINE=1
EXPERIMENTAL=0
PLATFORM=all
OUT_DIR="${REPO_DIR}/binaries"
BUILD_ROOT="${REPO_DIR}/build"
FIRMWARE=1
DOS_TOOLS=1
REQUIRE_DOS_TOOLS=0
VARIANT_ARGS=()
MERGE_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--experimental) EXPERIMENTAL=1; shift ;;
        --only-experimental) EXPERIMENTAL=1; MAINLINE=0; shift ;;
        -p|--platform) PLATFORM="$2"; shift 2 ;;
        -o|--out) OUT_DIR="$2"; shift 2 ;;
        -b|--build-dir) BUILD_ROOT="$2"; shift 2 ;;
        -j|--jobs) VARIANT_ARGS+=(--jobs "$2"); shift 2 ;;
        -c|--clean) VARIANT_ARGS+=(--clean); shift ;;
        --no-firmware) FIRMWARE=0; shift ;;
        --no-dos-tools) DOS_TOOLS=0; shift ;;
        --require-dos-tools) REQUIRE_DOS_TOOLS=1; shift ;;
        -r|--remote) MERGE_ARGS+=(--remote "$2"); shift 2 ;;
        -f|--fetch) MERGE_ARGS+=(--fetch); shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

# DOS programs that go into a release: each is a directory with a Makefile for
# OpenWatcom that leaves its .exe files next to it. Directories a source tree
# doesn't have are skipped, so experimental features can bring their own.
DOS_TOOL_DIRS=(pgusinit pgusdfs)

build_dos_tools() {
    local tree="$1" out="$2" dir exe
    if ! command -v wcl >/dev/null || [[ -z "${WATCOM:-}" ]]; then
        if [[ ${REQUIRE_DOS_TOOLS} -eq 1 ]]; then
            echo "OpenWatcom was not found (wcl on PATH and WATCOM set)" >&2
            return 1
        fi
        echo "==== DOS tools skipped: OpenWatcom was not found (wcl on PATH and WATCOM set)"
        return 0
    fi
    for dir in "${DOS_TOOL_DIRS[@]}"; do
        [[ -f "${tree}/${dir}/Makefile" ]] || continue
        echo "==== DOS tools: ${dir}"
        make -C "${tree}/${dir}" INCLUDE="${WATCOM}/h" > "${BUILD_ROOT}/dos-${dir}.log" 2>&1 \
            || { echo "build failed, see ${BUILD_ROOT}/dos-${dir}.log" >&2; tail -30 "${BUILD_ROOT}/dos-${dir}.log" >&2; return 1; }
        for exe in "${tree}/${dir}"/*.exe; do
            cp "${exe}" "${out}/"
            echo "  $(basename "${exe}")"
        done
    done
}

# The documentation that goes with the files: README.md is pgusinit's, as in
# every release so far; other DOS tools get <TOOL>.md. A source tree that
# carries experimental features (sw/experimental/<feature>.md) also gets
# EXPERIMENTAL.md, which says what is in it.
collect_docs() {
    local tree="$1" out="$2" dir file features=()
    cp "${tree}/pgusinit/README.md" "${out}/README.md"
    for dir in "${DOS_TOOL_DIRS[@]}"; do
        [[ "${dir}" != pgusinit && -f "${tree}/${dir}/README.md" ]] || continue
        cp "${tree}/${dir}/README.md" "${out}/$(echo "${dir}" | tr '[:lower:]' '[:upper:]').md"
    done
    for file in "${!FEATURE_DOCS[@]}"; do
        [[ -f "${tree}/${file}" ]] && cp "${tree}/${file}" "${out}/${FEATURE_DOCS[$file]}"
    done
    for file in "${tree}"/sw/experimental/*.md; do
        [[ -f "${file}" && "$(basename "${file}")" != README.md ]] && features+=("${file}")
    done
    rm -f "${out}/EXPERIMENTAL.md"
    [[ ${#features[@]} -gt 0 ]] || return 0
    {
        echo "# PicoGUS experimental build"
        echo
        echo "Built from \`$("${SCRIPT_DIR}/experimental.sh" source "${tree}")\`."
        echo "This is the PicoGUS firmware and its DOS programs with experimental features"
        echo "added that are not part of the regular releases:"
        echo
        echo '```'
        "${SCRIPT_DIR}/experimental.sh" describe "${tree}"
        echo '```'
        echo
        echo "The firmware reports them in its version (\`pgusinit\` shows it). Experimental"
        echo "features can change \`pgusinit.exe\` and how it talks to the card, so use the"
        echo "firmware files and DOS programs of this set together."
        for file in "${features[@]}"; do
            echo
            # Feature notes start at heading level 1; nest them under this document's title
            sed -e 's/^#/##/' "${file}"
        done
    } > "${out}/EXPERIMENTAL.md"
}

# Documents of experimental features that go with the files, where a source
# tree has them: path in the tree and the name it gets
declare -A FEATURE_DOCS=([sw/web/README.md]=WEBSERVER.md)

# build_set <source tree> <output dir> <build dir>
# The firmware images are build_variants.sh's default set: all modes and
# NE2000, plus what the tree's experimental features add
build_set() {
    local tree="$1" out="$2" build="$3"
    mkdir -p "${out}" "${build}"
    if [[ ${FIRMWARE} -eq 1 ]]; then
        "${tree}/sw/scripts/build_variants.sh" --platform "${PLATFORM}" --out "${out}" --build-dir "${build}" \
            ${VARIANT_ARGS[@]+"${VARIANT_ARGS[@]}"}
    fi
    if [[ ${DOS_TOOLS} -eq 1 ]]; then
        build_dos_tools "${tree}" "${out}"
        collect_docs "${tree}" "${out}"
    fi
}

mkdir -p "${OUT_DIR}" "${BUILD_ROOT}"

if [[ ${MAINLINE} -eq 1 ]]; then
    echo "######## This source tree ($("${SCRIPT_DIR}/experimental.sh" source))"
    build_set "${REPO_DIR}" "${OUT_DIR}" "${BUILD_ROOT}"
fi

if [[ ${EXPERIMENTAL} -eq 1 ]]; then
    echo "######## With the experimental features"
    EXP_SRC="${BUILD_ROOT}/experimental-src"
    "${SCRIPT_DIR}/experimental.sh" merge --worktree "${EXP_SRC}" ${MERGE_ARGS[@]+"${MERGE_ARGS[@]}"}
    build_set "${EXP_SRC}" "${OUT_DIR}/experimental" "${BUILD_ROOT}/experimental"
fi

echo
echo "Release files:"
(cd "${OUT_DIR}" && find . -type f | sort | sed -e 's/^\.\//  /')
