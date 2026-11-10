#!/bin/bash
# validate_topdown.sh — P4 top-down cross-check on a real Zen 4 / Zen 5 node.
#
#   1. encodings: every raw config in miniprof's top-down tables against
#      the config perf itself uses for the named event (perf stat -vv);
#   2. reference: perf stat -M PipelineL1 and PipelineL2 on
#      examples/topdown_validate (one process, MINIPROF_MODE=off);
#   3. miniprof: the same workload, MINIPROF_SNAPSHOT=topdown on 3 ranks;
#      level 1/2 compared with perf (whole run, compute scope), then the
#      per-region shape checks of tools/validate_topdown.py.
#
# perf multiplexes PipelineL2 (4 groups, ~25% each), so allow a few
# points of difference; miniprof's numbers are not multiplexed.
#
# Usage: tools/validate_topdown.sh [scale]     (from the repository root)
set -u
SCALE=${1:-1}
BIN=./examples/topdown_validate
OUT=miniprof_snapshot_td.json
[ -x "$BIN" ] || make examples/topdown_validate >/dev/null || exit 1
command -v perf >/dev/null || { echo "perf not found"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==================== 1. miniprof run (topdown, 3 ranks) ===================="
MINIPROF_MODE=snapshot MINIPROF_SNAPSHOT=topdown MINIPROF_SNAPSHOT_OUTPUT=$OUT \
    mpirun -n 3 --bind-to core "$BIN" "$SCALE" || exit 1

echo
echo "==================== 2. encodings vs perf ===================="
python3 - "$OUT" > "$TMP/events.txt" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))["metrics"]["counters"]
for s in c.get("sets", []):
    for e in s["events"]:
        print(s["name"], e["perf_name"], e["config"])
PY
NBAD=0
while read -r set name cfg; do
    ev=$name
    case $name in
        *,cmask=*)  # not a named event: build the cpu// form from the base event
            base=${name%%,cmask=*}; cm=${name##*cmask=}
            raw=$(perf stat -vv -e "$base" -- true 2>&1 | awk '$1=="config"{print $2; exit}')
            ev="cpu/config=$(printf '0x%x' $(( raw | (cm << 24) )))/" ;;
    esac
    pc=$(perf stat -vv -e "$ev" -- true 2>&1 | awk '$1=="config"{print $2; exit}')
    if [ -z "$pc" ]; then st="perf does not know this event"; NBAD=$((NBAD+1))
    elif [ $((pc)) -eq $((cfg)) ]; then st="ok"
    else st="MISMATCH: perf uses $pc"; NBAD=$((NBAD+1)); fi
    printf "  %-8s %-58s %-12s %s\n" "$set" "$name" "$cfg" "$st"
done < "$TMP/events.txt"
echo "  $NBAD encoding problem(s)"

echo
echo "==================== 3. perf stat -M PipelineL1 / PipelineL2 ===================="
for g in PipelineL1 PipelineL2; do
    MINIPROF_MODE=off perf stat -M $g -- "$BIN" "$SCALE" >/dev/null 2> "$TMP/$g.txt"
    grep -E '#' "$TMP/$g.txt"
done

echo
echo "==================== 4. miniprof vs perf (whole run, % of slots) ===================="
python3 - "$OUT" "$TMP/PipelineL1.txt" "$TMP/PipelineL2.txt" <<'PY'
import json, re, sys
td = json.load(open(sys.argv[1]))["metrics"]["topdown"]
perf = {}
for f in sys.argv[2:]:
    for m in re.finditer(r'#\s+([\d.]+)\s*%\S*\s+([a-z_]+)', open(f).read()):
        k = m.group(2).replace("_by_", "_").replace("_from_", "_")
        perf[k] = float(m.group(1))
alias = {"frontend_bound_latency": "frontend_latency", "frontend_bound_bandwidth": "frontend_bandwidth",
         "backend_bound_memory": "backend_memory", "backend_bound_cpu": "backend_cpu",
         "bad_speculation_mispredicts": "bad_speculation_mispredicts",
         "bad_speculation_pipeline_restarts": "bad_speculation_pipeline_restarts",
         "retiring_fastpath": "retiring_fastpath", "retiring_microcode": "retiring_microcode"}
perf = {alias.get(k, k): v for k, v in perf.items()}
if not td.get("available"):
    print("miniprof top-down unavailable:", td.get("reason")); sys.exit(0)
mine = dict(td["compute"]["level1"] or {}); mine.update(td["compute"]["level2"] or {})
bad = 0
print(f"  {'metric':<36} {'miniprof':>9} {'perf':>9} {'diff':>7}")
for k in ("frontend_bound", "bad_speculation", "backend_bound", "retiring", "smt_contention",
          "frontend_latency", "frontend_bandwidth", "bad_speculation_mispredicts",
          "bad_speculation_pipeline_restarts", "backend_memory", "backend_cpu",
          "retiring_fastpath", "retiring_microcode"):
    a, b = mine.get(k), perf.get(k)
    if a is None or b is None:
        print(f"  {k:<36} {'-' if a is None else f'{a:9.1f}':>9} {'-' if b is None else f'{b:9.1f}':>9}")
        continue
    d = a - b
    flag = "" if abs(d) <= 5 else "   <-- > 5 points"
    bad += abs(d) > 5
    print(f"  {k:<36} {a:9.1f} {b:9.1f} {d:+7.1f}{flag}")
print(f"  {bad} metric(s) differ by more than 5 points")
PY

echo
echo "==================== 5. per-region shapes ===================="
python3 tools/validate_topdown.py "$OUT"
