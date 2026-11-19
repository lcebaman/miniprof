#!/usr/bin/env python3
"""validate_threads.py — the snapshot of examples/threads_validate against
what the program measured itself (audit finding C1).

    validate_threads.py SNAPSHOT.json PROGRAM_OUTPUT.txt

Checks: descriptors do not grow with the number of threads that have come
and gone, the application can still open files, every exited thread is
counted, and the exited threads' counts are kept. The counted CPU time is
checked when the cycles event is the software task-clock (the CI stand-in,
MINIPROF_COUNTERS_TEST=1), where it is nanoseconds of CPU time: it must be
at least the threads' own measured CPU time (their counts were kept) and
at most the whole process's CPU time."""
import json, re, sys

fails = 0


def check(ok, msg):
    global fails
    print(("  ok    " if ok else "  FAIL  ") + msg)
    fails += 0 if ok else 1


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = json.load(open(sys.argv[1]))
    out = open(sys.argv[2]).read()
    m = re.search(r"threads=(\d+) fds_before=(-?\d+) fds_after=(-?\d+) open_ok=(\d) "
                  r"thread_cpu_ns=(\d+) process_cpu_ns=(\d+)", out)
    if not m:
        sys.exit("FAIL: no result line from threads_validate in " + sys.argv[2])
    n, before, after, ok, tcpu, pcpu = (int(x) for x in m.groups())
    print(f"threads_validate: {n} short threads; descriptors {before} -> {after}")

    # -1: the program could not even open /proc/self/fd to count them
    check(before >= 0 and after >= 0 and after - before <= 4,
          f"descriptors do not grow with exited threads ({before} -> {after})")
    check(ok == 1, "the application can still open files")

    c = (d.get("metrics") or {}).get("counters") or {}
    if not c.get("available"):
        print(f"  skip  counters not available: {c.get('reason')}")
        sys.exit(1 if fails else 0)
    check(c.get("threads_dropped") == 0, f"no thread dropped ({c.get('threads_dropped')})")
    counted = c.get("threads_counted")
    check(counted is not None and counted >= n, f"every exited thread counted ({counted} >= {n})")

    ev = {e["key"]: e for e in c.get("events") or []}
    cyc = ev.get("cycles") or {}
    if cyc.get("perf_name") == "task-clock":
        tot = (c.get("totals") or {}).get("cycles")
        check(tot is not None and tot >= 0.95 * tcpu,
              f"counted CPU time {tot / 1e6:.1f} ms >= the threads' own {tcpu / 1e6:.1f} ms (exited threads kept)")
        check(tot is not None and tot <= 1.05 * pcpu,
              f"counted CPU time {tot / 1e6:.1f} ms <= the process's {pcpu / 1e6:.1f} ms (nothing counted twice)")
    else:
        print("  skip  CPU-time cross-check needs the task-clock stand-in (MINIPROF_COUNTERS_TEST=1)")
    print("PASS" if not fails else f"FAIL ({fails} checks)")
    sys.exit(1 if fails else 0)


main()
