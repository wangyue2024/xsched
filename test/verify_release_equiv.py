#!/usr/bin/env python3
"""Verify the refactored working tree is CODE-IDENTICAL to origin/release/sm120-level2.
Only comments/tests should differ; every kept source file's code must match exactly."""
import subprocess
import re
import sys

REL = "origin/release/sm120-level2"   # bb81e4d - the original (pre-cleanup) release

# Files I edited (reverted+reapplied, or comment-stripped). Their CODE must equal REL.
EDITED = [
    "platforms/cuda/shim/src/shim.cpp",
    "platforms/cuda/hal/src/level2/mm.cpp",
    "platforms/cuda/hal/include/xsched/cuda/hal/level2/guardian.h",
    "platforms/cuda/hal/src/arch/arch.cpp",
]
# Files reverted to baseline (must NOT exist as changes vs baseline; code = baseline).
REVERTED = [
    "include/xsched/types.h",
    "include/xsched/hint.h",
    "include/xsched/xqueue.h",
]
# Files removed from release (must be absent in working tree).
REMOVED = [
    "tests_local/run_tests.ps1",
    "tests_local/test_cuda_driver.cpp",
    "tests_local/test_pytorch.py",
    "manage_xsched.ps1",
    "tools/instrument/extract_sass.py",
    "tools/instrument/README.md",
    "platforms/cuda/hal/inject/make_msvc.bat",
]

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    lines = [re.sub(r"\s+", " ", ln).strip() for ln in src.split("\n")]
    return [ln for ln in lines if ln]

def git_show(rev, path):
    r = subprocess.run(["git", "show", f"{rev}:{path}"], capture_output=True)
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None

def read_worktree(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return None

ok = True
print("=" * 78)
print("CHECK 1: edited files must be CODE-IDENTICAL to original release")
print("=" * 78)
for path in EDITED:
    rel_code = strip_comments(git_show(REL, path) or "")
    wt_code = strip_comments(read_worktree(path) or "")
    match = (rel_code == wt_code)
    ok = ok and match
    print(f"  [{'OK ' if match else 'FAIL'}] {path}")
    if not match:
        # show first divergence
        for i, (a, b) in enumerate(zip(rel_code, wt_code)):
            if a != b:
                print(f"        line {i}: REL={a!r}")
                print(f"                 WT ={b!r}")
                break
        print(f"        len REL={len(rel_code)} WT={len(wt_code)}")

print()
print("=" * 78)
print("CHECK 2: reverted headers must be IDENTICAL to baseline f49289f")
print("=" * 78)
for path in REVERTED:
    base = git_show("f49289f", path)
    wt = read_worktree(path)
    match = (base == wt)
    ok = ok and match
    print(f"  [{'OK ' if match else 'FAIL'}] {path} (full-file identical to baseline)")

print()
print("=" * 78)
print("CHECK 3: removed files must be ABSENT from working tree")
print("=" * 78)
for path in REMOVED:
    absent = (read_worktree(path) is None)
    ok = ok and absent
    print(f"  [{'OK ' if absent else 'FAIL'}] {path} (absent)")

print()
print("=" * 78)
print("VERDICT:", "ALL CHECKS PASSED - code preserved, only comments/tests removed"
      if ok else "*** FAILURE - investigate before committing ***")
print("=" * 78)
sys.exit(0 if ok else 1)
