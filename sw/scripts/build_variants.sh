#!/usr/bin/env bash
# Build PicoGUS firmware variants for RP2040 and/or RP2350.
#
# Every chip/variant pair gets its own CMake build directory, and the UF2s are
# collected into one output directory. RP2040 files keep their historical names
# (picogus.uf2, pg-ne2k.uf2, ...); RP2350 files get an "-rp2350" suffix
# (picogus-rp2350.uf2, pg-ne2k-rp2350.uf2, ...). The "picogus21" target is the
# RP2350B PicoGUS v2.1 THT board (PICO_BOARD=picogus21), with a "-v21" suffix
# (picogus-v21.uf2, ...).
#
# Needs PICO_SDK_PATH and PICO_EXTRAS_PATH like a normal SDK build. Uses
# CMake's default generator unless CMAKE_GENERATOR is set (e.g. to Ninja);
# stick to one generator, as the SDK's picotool fetch directory is shared.

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
    [analyzer]=ANALYZER            [sb-ne2k]=SB_NE2K
)
declare -A OUTPUTS=(
    [multifw]=picogus              [ne2k]=pg-ne2k
    [gus]=pg-gus                   [mpu]=pg-mpu
    [psg]=pg-psg                   [usb]=pg-usb
    [sb]=pg-sb-dbopl3              [sb-ymf262]=pg-sb-ymf262
    [sb-ym3812]=pg-sb-ym3812       [sb-emu8950]=pg-sb-emu8950
    [adlib]=pg-adlib               [adlib-ymf262]=pg-adlib-ymf262
    [adlib-ym3812]=pg-adlib-ym3812 [adlib-dbopl3]=pg-adlib-dbopl3
    [analyzer]=pg-analyzer         [sb-ne2k]=pg-sb-ne2k
)
ALL_VARIANTS=(multifw ne2k gus sb sb-ymf262 sb-ym3812 sb-emu8950 adlib adlib-ymf262 adlib-ym3812 adlib-dbopl3 mpu psg usb analyzer sb-ne2k)
DEFAULT_VARIANTS=(multifw ne2k)
# The ISA analyzer uses RP2350-only hardware (QMI PSRAM); SB + NE2000 is an
# RP2350 experiment (sw/docs/sb-ne2000-rp2350.md)
declare -A CHIP_ONLY=([analyzer]=rp2350 [sb-ne2k]=rp2350)

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] [variant...]

Builds the given firmware variants (default: ${DEFAULT_VARIANTS[*]}) for each chip.

Options:
  -p, --platform CHIP   rp2040, rp2350, picogus21 (RP2350B PicoGUS v2.1) or all (default: all)
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
                printf '%-14s PROJECT_TYPE=%-13s %s.uf2%s\n' "$v" "${PROJECT_TYPES[$v]}" "${OUTPUTS[$v]}" \
                    "${CHIP_ONLY[$v]:+ (${CHIP_ONLY[$v]} only)}"
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
    local dest_name="${out_name}.uf2"
    local platform="${chip}" board_args=()
    [[ "${chip}" == rp2350 ]] && dest_name="${out_name}-rp2350.uf2"
    if [[ "${chip}" == picogus21 ]]; then
        platform=rp2350
        board_args=(-DPICO_BOARD=picogus21)
        dest_name="${out_name}-v21.uf2"
    fi

    echo "==== ${chip} ${variant} (PROJECT_TYPE=${PROJECT_TYPES[$variant]}) -> ${dest_name}"
    [[ ${CLEAN} -eq 1 ]] && rm -rf "${build_dir}"
    cmake -S "${SW_DIR}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPICO_PLATFORM="${platform}" "${board_args[@]}" \
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
        only="${CHIP_ONLY[$variant]:-}"
        if [[ "${chip}" == picogus21 && "${variant}" == analyzer ]]; then
            echo "==== ${chip} ${variant}: skipped (Pico Plus 2 tool)"
            continue
        fi
        [[ "${only}" == rp2350 && "${chip}" == picogus21 ]] && only=picogus21
        if [[ -n "${only}" && "${only}" != "${chip}" ]]; then
            echo "==== ${chip} ${variant}: skipped (${only} only)"
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
