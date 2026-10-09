#!/usr/bin/env bash
# =====================================================================
#  seal_t7.sh - T7 evidence sealing (SHA256SUMS generator, Linux)
#
#  Linux counterpart of seal_t7.ps1 -- same entry set, same order:
#    A: files under test/sm120_integration (sources + logs; work/ excluded)
#    B: changed platform files (the integration itself)
#    C: environment-boundary investigation tools, integration scripts
#       and the MVE session logs bound by the report
#  Emits evidence/SHA256SUMS_T7.txt in sha256sum format (LF, ascii).
#  Re-runnable; the output file itself is excluded from the manifest.
#
#  Usage:
#    ./seal_t7.sh
# =====================================================================
set -eu
export LC_ALL=C

INT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$INT_DIR/../.." && pwd)"
SUMS="$INT_DIR/evidence/SHA256SUMS_T7.txt"

rel() { printf '%s\n' "${1#$REPO_ROOT/}"; }
add() {
    local full="$REPO_ROOT/$1"
    if [ -f "$full" ]; then
        printf '%s  %s\n' "$(sha256sum "$full" | cut -d' ' -f1)" "$1"
    else
        printf '# MISSING: %s\n' "$1"
    fi
}

{
    # ---- A: files under test/sm120_integration (work/ excluded) ----
    find "$INT_DIR" -type f ! -name 'SHA256SUMS_T7.txt' -not -path "$INT_DIR/work/*" -print0 \
        | sort -z \
        | while IFS= read -r -d '' f; do
              printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(rel "$f")"
          done

    # ---- B: changed platform files ----
    for p in \
        'platforms/cuda/hal/include/xsched/cuda/hal/level2/guardian.h' \
        'platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h' \
        'platforms/cuda/hal/src/arch/sm120.cpp' \
        'platforms/cuda/hal/src/arch/arch.cpp' \
        'platforms/cuda/hal/src/level2/instrument.cpp'
    do add "$p"; done

    # ---- C: investigation tools + MVE logs ----
    # (the integration scripts build.sh/run.sh/seal_t7.sh need no extra
    #  entry: section A already covers every file in this directory)
    for p in \
        'test/sm120_mve/pure_cuda_check.cu' \
        'test/sm120_mve/mve_m0_repro.cu' \
        'test/sm120_mve/mve_main.cpp' \
        'test/sm120_mve/run_mve.ps1' \
        'test/sm120_mve/run_mve.sh'
    do add "$p"; done

    find "$REPO_ROOT/test/sm120_mve/evidence" -maxdepth 1 -type f -print0 2>/dev/null \
        | sort -z \
        | while IFS= read -r -d '' f; do
              case "$(basename "$f")" in
                  mve_t7_regression*|mve_stability*|mve_diag*|mve_preflight*)
                      printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(rel "$f")" ;;
              esac
          done
} > "$SUMS"

echo "sealed: $(wc -l < "$SUMS") entries -> $SUMS"
