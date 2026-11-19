#!/usr/bin/env python3
"""The tools must run on the oldest python3 a site may have: 3.6 (RHEL 8,
SLES 15). Parse every tool and test with that grammar; a construct from a
later Python (":=", positional-only "/", ...) fails here instead of at a
site, where it would stop the report after every job. Standard library
only; needs Python >= 3.8 to run (feature_version)."""
import ast, glob, os, sys

MIN = (3, 6)
if sys.version_info < (3, 8):
    print(f"SKIP: python {sys.version_info[0]}.{sys.version_info[1]} parses with its own grammar")
    sys.exit(0)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
files = sorted(glob.glob(os.path.join(root, "tools", "*.py")) + glob.glob(os.path.join(root, "tests", "*.py")))
bad = 0
for f in files:
    try:
        ast.parse(open(f).read(), f, feature_version=MIN)
    except SyntaxError as e:
        bad += 1
        print(f"FAIL: {os.path.relpath(f, root)}:{e.lineno}: {e.msg}")
print(f"{'PASS' if not bad else 'FAIL'}: {len(files)} Python files parse as Python {MIN[0]}.{MIN[1]}")
sys.exit(1 if bad else 0)
