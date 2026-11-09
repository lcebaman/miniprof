#!/usr/bin/env python3
"""Re-parse test_units' JSON output with Python's json module and check
the values round-trip exactly — an independent check that json_writer.c
emits valid JSON, not just output it considers self-consistent."""
import json
import sys

d = json.load(open(sys.argv[1]))
assert d["plain"] == "hello"
assert d["escaped"] == 'quote" backslash\\ newline\n tab\t ctrl\x01', repr(d["escaped"])
assert d["null_str"] is None and d["nan"] is None and d["inf"] is None
assert d["big"] == 1234567890123 and d["neg"] == -42
assert abs(d["pi"] - 3.14159265358979) < 1e-11
assert d["t"] is True and d["f"] is False and d["n"] is None
assert d["empty_arr"] == [] and d["empty_obj"] == {}
assert d["arr"] == [1, {"k": "v"}, [0.5]]
print("PASS: JSON writer output re-parses with Python json, values round-trip")
