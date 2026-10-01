# XSched inject toolchain Makefile (modernized for multi-arch generation)
#
# Usage:
#   make ARCH=120 bin dump cc     # explicit target arch
#   make bin dump cc              # auto-detect from local GPU (nvidia-smi)
#
# Artifacts:
#   inject_$(ARCH).cubin     nvcc -cubin (keep-device-functions + astoolspatch)
#   inject_$(ARCH).asm       cuobjdump -sass disassembly (manual review file)
#   inject_$(ARCH)_cc.asm    nvdisasm disassembly (code only)
#
# Notes:
#   - Requires CUDA Toolkit >= 12.8 for sm_120 (CUDA >= 12.9 recommended).
#   - ARCH is the compute capability digits without the dot, e.g. 86 / 120.
#   - On Windows, use make_msvc.bat (activates MSVC host env, then calls this
#     Makefile):  make_msvc.bat ARCH=120 bin dump cc
#   - nvdisasm note (CUDA 12.9): passing '-b SM120' mis-parses sm_120 cubins
#     (1871 spurious "Unrecognized operation for functional unit 'uC'" errors),
#     while '-b SM120A' or architecture auto-detection yields 0 errors.
#     The 'cc' target therefore relies on auto-detection (no -b flag).

ARCH ?= $(strip $(subst .,,$(shell nvidia-smi --query-gpu=compute_cap --format=csv,noheader)))

ifeq ($(ARCH),)
$(error ARCH not specified and GPU auto-detect failed; use: make ARCH=120)
endif

CUBIN  = inject_$(ARCH).cubin
ASM    = inject_$(ARCH).asm
CCASM  = inject_$(ARCH)_cc.asm

NVCC      ?= nvcc
CUOBJDUMP ?= cuobjdump
NVDISASM  ?= nvdisasm

.PHONY: all bin dump cc clean

all: bin dump cc

bin: $(CUBIN)

$(CUBIN): inject.cu
	$(NVCC) -cubin $< -o $@ --keep-device-functions -Xptxas -astoolspatch -arch=sm_$(ARCH)

dump: $(ASM)

$(ASM): $(CUBIN)
	$(CUOBJDUMP) -sass $(CUBIN) > $@

cc: $(CCASM)

$(CCASM): $(CUBIN)
	$(NVDISASM) -c $(CUBIN) > $@

clean:
	-cmd /c del /q $(CUBIN) $(ASM) $(CCASM)
