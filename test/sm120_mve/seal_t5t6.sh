#!/usr/bin/env bash
# =====================================================================
#  seal_t5t6.sh - T5/T6 evidence sealing (SHA256SUMS generator, Linux)
#
#  Linux counterpart of seal_t5t6.ps1 -- same entry set, same order:
#    A: every file under evidence/ (recursive, excluding the manifest)
#    B: T5 platform deliverables (sm120.h / sm120.cpp)
#    C: MVE sources, scripts and docs
#    D: MVE build artifacts
#  Emits evidence/SHA256SUMS_T5T6.txt in sha256sum format (LF, ascii).
#  Re-runnable; the output file itself is excluded from the manifest.
#
#  Usage:
#    ./seal_t5t6.sh
# =====================================================================
set -eu
export LC_ALL=C

MVE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$MVE_DIR/../.." && pwd)"
SUMS="$MVE_DIR/evidence/SHA256SUMS_T5T6.txt"

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
    # ---- A: every file under evidence/ (except the sums file itself) ----
    find "$MVE_DIR/evidence" -type f ! -name 'SHA256SUMS_T5T6.txt' -print0 \
        | sort -z \
        | while IFS= read -r -d '' f; do
              printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(rel "$f")"
          done

    # ---- B: T5 platform deliverables ----
    for p in \
        'platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h' \
        'platforms/cuda/hal/src/arch/sm120.cpp'
    do add "$p"; done

    # ---- C: MVE sources, scripts and docs ----
    for p in \
        'test/sm120_mve/mve_main.cpp' \
        'test/sm120_mve/dummy_kernel.cu' \
        'test/sm120_mve/mve_arrays.h' \
        'test/sm120_mve/gen_arrays.py' \
        'test/sm120_mve/build.ps1' \
        'test/sm120_mve/run_mve.ps1' \
        'test/sm120_mve/build.sh' \
        'test/sm120_mve/run_mve.sh' \
        'test/sm120_mve/t5_verify.py' \
        'test/sm120_mve/seal_t5t6.ps1' \
        'test/sm120_mve/seal_t5t6.sh' \
        'test/sm120_mve/DESIGN.md' \
        'test/sm120_mve/T5_REVIEW.md' \
        'test/sm120_mve/REPORT.md'
    do add "$p"; done

    # ---- D: MVE build artifacts ----
    for p in \
        'test/sm120_mve/mve_kernel.cubin' \
        'test/sm120_mve/mve_kernel.sass.txt'
    do add "$p"; done
} > "$SUMS"

echo "sealed: $(wc -l < "$SUMS") entries -> $SUMS"
