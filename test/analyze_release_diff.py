#!/usr/bin/env python3
"""Line-level classification of every diff line: comment/blank vs real code.
Gives the definitive comment-churn ratio for the release/sm120-level2 diff."""
import subprocess
import re

BASE = "f49289f"
REL = "origin/release/sm120-level2"

def run(args):
    return subprocess.run(args, capture_output=True).stdout.decode("utf-8", "replace")

# Classify a single source line (in isolation) as comment/blank vs code.
def is_comment_or_blank(line):
    s = line.strip()
    if not s:
        return True
    if s.startswith("//"):
        return True
    if s.startswith("/*") or s.startswith("*") or s.startswith("*/"):
        return True
    return False

# Get per-file unified diffs (only for MODIFIED files, not new files)
numstat = run(["git", "diff", "--numstat", BASE, REL]).strip().split("\n")

modified = []
new_files = []
for line in numstat:
    p = line.split("\t")
    if len(p) < 3:
        continue
    path = p[2]
    base_exists = subprocess.run(["git", "cat-file", "-e", f"{BASE}:{path}"],
                                 capture_output=True).returncode == 0
    if base_exists:
        modified.append(path)
    else:
        new_files.append((path, int(p[0]) if p[0] != "-" else 0))

print("=" * 100)
print("MODIFIED FILES: comment-churn vs real-code churn (line-level classification)")
print("=" * 100)
print(f"{'FILE':<54}{'cmt+/-':>10}{'code+/-':>10}{'  verdict':>16}")
print("-" * 100)

grand_cmt = 0
grand_code = 0
for path in modified:
    diff = run(["git", "diff", "-U0", BASE, REL, "--", path])
    cmt_add = cmt_del = code_add = code_del = 0
    for ln in diff.split("\n"):
        if ln.startswith("+++") or ln.startswith("---") or ln.startswith("@@"):
            continue
        if ln.startswith("+"):
            if is_comment_or_blank(ln[1:]):
                cmt_add += 1
            else:
                code_add += 1
        elif ln.startswith("-"):
            if is_comment_or_blank(ln[1:]):
                cmt_del += 1
            else:
                code_del += 1
    cmt = cmt_add + cmt_del
    code = code_add + code_del
    grand_cmt += cmt
    grand_code += code
    if code == 0:
        verdict = "PURE-COMMENT"
    elif cmt > code * 2:
        verdict = "comment-heavy"
    else:
        verdict = "code"
    short = path if len(path) <= 53 else "..." + path[-50:]
    print(f"{short:<54}{cmt:>7}   {code:>7}   {verdict:>14}")

print("-" * 100)
print(f"{'MODIFIED-FILE TOTAL':<54}{grand_cmt:>7}   {grand_code:>7}")
print()
print(f"NEW FILES (all additions, feature/tools/tests/docs): {len(new_files)} files, "
      f"{sum(n for _, n in new_files)} lines")
for path, n in sorted(new_files, key=lambda x: -x[1]):
    print(f"    +{n:<5} {path}")
print()
print("=" * 100)
print("DEFINITIVE SUMMARY")
print("=" * 100)
print(f"  Comment/blank diff lines in MODIFIED files : {grand_cmt}")
print(f"  Real code diff lines in MODIFIED files      : {grand_code}")
print(f"  => Comment churn ratio (modified files)     : "
      f"{100*grand_cmt//(grand_cmt+grand_code)}%")
