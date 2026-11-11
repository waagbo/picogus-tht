#!/usr/bin/env bash
# Build PicoGUS firmware variants for RP2040 and/or RP2350 from this source tree.
#
# Every platform/variant pair gets its own CMake build directory, and the UF2s
# are collected into one output directory. The platforms are
#   rp2040     Pico, Pico W and PicoGUS 2.0: the files keep their historical
#              names (picogus.uf2, pg-ne2k.uf2, ...)
#   rp2350     Pico 2 and Pico 2 W: pgus2350.uf2 and ne2k2350.uf2
#   picogus21  the RP2350B PicoGUS v2.1 THT board (PICO_BOARD=picogus21):
#              core2350.uf2
# The RP2350 release files have names that stay intact on a DOS (8.3) file
# system; the other variants get an "-rp2350" or "-v21" suffix.
#
# Some variants only exist in source trees that carry an experimental feature
# (sw/experimental/README.md), and are part of the default set there: "web",
# all modes with the web server for Pico W and Pico 2 W boards, is
# picogusw.uf2 and pg2350w.uf2.
#
# Needs PICO_SDK_PATH and PICO_EXTRAS_PATH like a normal SDK build. Uses
# CMake's default generator unless CMAKE_GENERATOR is set (e.g. to Ninja);
# stick to one generator, as the SDK's picotool fetch directory is shared.
#
# sw/scripts/build_release.sh builds the release set with this script, for the
# mainline firmware and for the experimental flavor.

set -euo pipefail

if (( BASH_VERSINFO[0] < 4 )); then
    echo "$(basename "$0") needs bash 4 or newer" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${SW_DIR}/.." && pwd)"

# variant name -> PROJECT_TYPE and the UF2 the build produces
declare -A PROJECT_TYPES=(
    [multifw]=MULTIFW              [ne2k]=NE2K
    [gus]=GUS                      [mpu]=MPU
    [psg]=PSG                      [usb]=USB
    [sb]=SB_DBOPL3                 [sb-ymf262]=SB_YMF262
    [sb-ym3812]=SB_YM3812          [sb-emu8950]=SB_EMU8950
    [adlib]=ADLIB                  [adlib-ymf262]=ADLIB_YMF262
    [adlib-ym3812]=ADLIB_YM3812    [adlib-dbopl3]=ADLIB_DBOPL3
    [analyzer]=ANALYZER            [web]=MULTIFW
)
declare -A OUTPUTS=(
    [multifw]=picogus              [ne2k]=pg-ne2k
    [gus]=pg-gus                   [mpu]=pg-mpu
    [psg]=pg-psg                   [usb]=pg-usb
    [sb]=pg-sb-dbopl3              [sb-ymf262]=pg-sb-ymf262
    [sb-ym3812]=pg-sb-ym3812       [sb-emu8950]=pg-sb-emu8950
    [adlib]=pg-adlib               [adlib-ymf262]=pg-adlib-ymf262
    [adlib-ym3812]=pg-adlib-ym3812 [adlib-dbopl3]=pg-adlib-dbopl3
    [analyzer]=pg-analyzer         [web]=picogus
)
# Names of the collected files where they differ from what the build calls
# them; otherwise RP2350 files are <RP2040 name>-rp2350 or <RP2040 name>-v21
declare -A RP2040_OUTPUTS=(
    [web]=picogusw
)
declare -A RP2350_OUTPUTS=(
    [multifw]=pgus2350             [ne2k]=ne2k2350
    [web]=pg2350w
)
declare -A PICOGUS21_OUTPUTS=(
    [multifw]=core2350
)
ALL_VARIANTS=(multifw ne2k gus sb sb-ymf262 sb-ym3812 sb-emu8950 adlib adlib-ymf262 adlib-ym3812 adlib-dbopl3 mpu psg usb analyzer)
DEFAULT_VARIANTS=(multifw ne2k)
# The ISA analyzer is written for RP2350 boards (QMI registers, optional QMI PSRAM)
declare -A CHIP_ONLY=([analyzer]=rp2350)
# Variants that need a Pico W or Pico 2 W (CYW43), so not for the v2.1 board
declare -A NEEDS_PICOW=([ne2k]=1 [web]=1)
# Variants built for the Pico W / Pico 2 W board rather than the platform's
# default board (the NE2K project type picks that board by itself)
declare -A PICOW_BOARDS=([rp2040]=picow_fast [rp2350]=pico2w_fast)
declare -A ON_PICOW_BOARD=([web]=1)
# Variants of experimental features: the directory under sw/ the feature adds.
# They join the lists above in source trees that have it.
declare -A FEATURE_DIR=([web]=web)
for variant in "${!FEATURE_DIR[@]}"; do
    if [[ -d "${SW_DIR}/${FEATURE_DIR[$variant]}" ]]; then
        ALL_VARIANTS+=("${variant}")
        DEFAULT_VARIANTS+=("${variant}")
    fi
done

# Name of the collected UF2 for a chip/variant pair
dest_name() {
    local chip="$1" variant="$2"
    case "${chip}" in
        rp2350) echo "${RP2350_OUTPUTS[$variant]:-${OUTPUTS[$variant]}-rp2350}.uf2" ;;
        picogus21) echo "${PICOGUS21_OUTPUTS[$variant]:-${OUTPUTS[$variant]}-v21}.uf2" ;;
        *) echo "${RP2040_OUTPUTS[$variant]:-${OUTPUTS[$variant]}}.uf2" ;;
    esac
}

# Why a variant isn't built for a platform, or nothing if it is
skip_reason() {
    local chip="$1" variant="$2" only="${CHIP_ONLY[$2]:-}"
    if [[ -n "${FEATURE_DIR[$variant]:-}" && ! -d "${SW_DIR}/${FEATURE_DIR[$variant]}" ]]; then
        echo "this source tree has no sw/${FEATURE_DIR[$variant]}"
    elif [[ "${chip}" == picogus21 ]]; then
        if [[ -n "${NEEDS_PICOW[$variant]:-}" ]]; then
            echo "needs a Pico W"
        elif [[ -n "${only}" ]]; then
            echo "not for the v2.1 board"
        fi
    elif [[ -n "${only}" && "${only}" != "${chip}" ]]; then
        echo "${only} only"
    fi
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] [variant...]

Builds the given firmware variants (default: ${DEFAULT_VARIANTS[*]}) for each platform.

Options:
  -p, --platform NAME   rp2040, rp2350, picogus21 (RP2350B PicoGUS v2.1) or all (default: all)
  -o, --out DIR         where to collect UF2 files (default: ${REPO_DIR}/binaries)
  -b, --build-dir DIR   parent of the per-build directories (default: ${REPO_DIR}/build)
  -j, --jobs N          parallel build jobs (default: number of CPUs)
  -d, --debug           build with the PGDEBUG printf framework enabled
  -c, --clean           delete each build directory before configuring
  -k, --keep-going      continue with the remaining builds after a failure
  -l, --list            list the variants and exit
  -h, --help            show this help

Variants: all ${ALL_VARIANTS[*]}
EOF
}

PLATFORMS=(rp2040 rp2350 picogus21)
OUT_DIR="${REPO_DIR}/binaries"
BUILD_ROOT="${REPO_DIR}/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
PGDEBUG=OFF
CLEAN=0
KEEP_GOING=0
VARIANTS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--platform)
            case "$2" in
                all) PLATFORMS=(rp2040 rp2350 picogus21) ;;
                rp2040|rp2350|picogus21) PLATFORMS=("$2") ;;
                *) echo "Unknown platform '$2' (expected rp2040, rp2350, picogus21 or all)" >&2; exit 2 ;;
            esac
            shift 2 ;;
        -o|--out) OUT_DIR="$2"; shift 2 ;;
        -b|--build-dir) BUILD_ROOT="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        -d|--debug) PGDEBUG=ON; shift ;;
        -c|--clean) CLEAN=1; shift ;;
        -k|--keep-going) KEEP_GOING=1; shift ;;
        -l|--list)
            for v in "${ALL_VARIANTS[@]}"; do
                names=()
                for chip in rp2040 rp2350 picogus21; do
                    [[ -z "$(skip_reason "${chip}" "$v")" ]] && names+=("$(dest_name "${chip}" "$v")")
                done
                printf '%-14s PROJECT_TYPE=%-13s %s\n' "$v" "${PROJECT_TYPES[$v]}" "${names[*]}"
            done
            exit 0 ;;
        -h|--help) usage; exit 0 ;;
        all) VARIANTS+=("${ALL_VARIANTS[@]}"); shift ;;
        -*) echo "Unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)
            if [[ -z "${PROJECT_TYPES[$1]+x}" ]]; then
                echo "Unknown variant '$1'" >&2; usage >&2; exit 2
            fi
            VARIANTS+=("$1"); shift ;;
    esac
done
[[ ${#VARIANTS[@]} -eq 0 ]] && VARIANTS=("${DEFAULT_VARIANTS[@]}")

if [[ -z "${PICO_SDK_PATH:-}" ]]; then
    echo "PICO_SDK_PATH is not set" >&2
    exit 2
fi

mkdir -p "${OUT_DIR}"
RESULTS=()
FAILED=0

build_one() {
    local chip="$1" variant="$2"
    local build_dir="${BUILD_ROOT}/${chip}/${variant}"
    local out_name="${OUTPUTS[$variant]}"
    local dest_name platform="${chip}" board_args=()
    dest_name="$(dest_name "${chip}" "${variant}")"
    if [[ "${chip}" == picogus21 ]]; then
        platform=rp2350
        board_args=(-DPICO_BOARD=picogus21)
    elif [[ -n "${ON_PICOW_BOARD[$variant]:-}" ]]; then
        board_args=(-DPICO_BOARD="${PICOW_BOARDS[$chip]}")
    fi

    echo "==== ${chip} ${variant} (PROJECT_TYPE=${PROJECT_TYPES[$variant]}) -> ${dest_name}"
    [[ ${CLEAN} -eq 1 ]] && rm -rf "${build_dir}"
    cmake -S "${SW_DIR}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPICO_PLATFORM="${platform}" ${board_args[@]+"${board_args[@]}"} \
        -DPROJECT_TYPE="${PROJECT_TYPES[$variant]}" \
        -DPGDEBUG="${PGDEBUG}" \
        > "${build_dir}.configure.log" 2>&1 \
        || { echo "configure failed, see ${build_dir}.configure.log" >&2; tail -20 "${build_dir}.configure.log" >&2; return 1; }
    cmake --build "${build_dir}" --parallel "${JOBS}" \
        > "${build_dir}.build.log" 2>&1 \
        || { echo "build failed, see ${build_dir}.build.log" >&2; tail -30 "${build_dir}.build.log" >&2; return 1; }
    cp "${build_dir}/${out_name}.uf2" "${OUT_DIR}/${dest_name}"
    RESULTS+=("$(printf '%-7s %-13s %-30s %8d bytes' "${chip}" "${variant}" "${dest_name}" "$(stat -c %s "${OUT_DIR}/${dest_name}")")")
}

mkdir -p "${BUILD_ROOT}"
for chip in "${PLATFORMS[@]}"; do
    mkdir -p "${BUILD_ROOT}/${chip}"
    for variant in "${VARIANTS[@]}"; do
        reason="$(skip_reason "${chip}" "${variant}")"
        if [[ -n "${reason}" ]]; then
            echo "==== ${chip} ${variant}: skipped (${reason})"
            continue
        fi
        if ! build_one "${chip}" "${variant}"; then
            FAILED=$((FAILED + 1))
            RESULTS+=("$(printf '%-7s %-13s FAILED' "${chip}" "${variant}")")
            [[ ${KEEP_GOING} -eq 1 ]] || break 2
        fi
    done
done

echo
echo "Summary (UF2 files in ${OUT_DIR}):"
printf '  %s\n' "${RESULTS[@]}"
if [[ ${FAILED} -gt 0 ]]; then
    echo "${FAILED} build(s) failed" >&2
    exit 1
fi
