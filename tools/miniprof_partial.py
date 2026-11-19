#!/usr/bin/env python3
"""miniprof_partial.py — a snapshot from the per-rank records left by a run
that ended without MPI_Finalize (time limit, SIGINT, MPI_Abort, exit).

    miniprof_partial.py SNAPSHOT.json.partial/ [-o SNAPSHOT.json]

Each rank that could write one left <snapshot>.partial/rank.<r>.json (see
src/partial.h). This merges whatever is there into a snapshot JSON with the
same structure, so miniprof_report.py, the HTML and CSV reports and
miniprof_diff.py read it as usual. It is marked partial: run.partial says
why the run ended, which ranks are missing, and what a partial snapshot
cannot have (only what each rank could write alone: the time split, MPI by
function and message size, I/O, CPU, and hardware counters at the basic,
roofline and full levels; no top-down, memory, roofline, OpenMP, hotspots
or timeline). The definitions match the ones in src/snapshot.c.

Default output: the snapshot path the run would have written (the
directory name without ".partial")."""
import argparse, glob, json, os, sys

CLASSES = ("p2p", "collective", "wait", "rma", "comm")


def stat(xs):
    xs = list(xs)
    if not xs:
        return {"min": 0, "avg": 0, "max": 0}
    return {"min": min(xs), "avg": sum(xs) / len(xs), "max": max(xs)}


def pct(part, whole):
    return 100.0 * part / whole if whole > 0 else 0.0


def imb(s):
    return 100.0 * (s["max"] - s["avg"]) / s["max"] if s["max"] > 0 else 0.0


def ratio(a, b):
    return a / b if b > 0 else 0.0


def load(d):
    recs, bad = [], []
    for f in sorted(glob.glob(os.path.join(d, "rank.*.json"))):
        try:
            r = json.load(open(f))
            if r.get("schema") == "miniprof-partial":
                recs.append(r)
        except (OSError, ValueError):
            bad.append(os.path.basename(f))
    # one run only: keep the records of the most common start (a stale record
    # from an earlier run with the same path would otherwise be merged in)
    if recs:
        starts = {}
        for r in recs:
            starts[r.get("start_utc")] = starts.get(r.get("start_utc"), 0) + 1
        main = max(starts, key=starts.get)
        bad += [f"rank.{r['rank']}.json (another run, started {r.get('start_utc')})"
                for r in recs if r.get("start_utc") != main]
        recs = [r for r in recs if r.get("start_utc") == main]
    return sorted(recs, key=lambda r: r["rank"]), bad


def io_ns(r):
    return sum(r["io"][k][t] for k in ("posix", "mpiio") for t in ("read_ns", "write_ns", "meta_ns"))


def merge(recs, bad):
    n = len(recs)
    nranks = max(r["nranks"] for r in recs)
    for r in recs:                                  # per-rank times, as collect_parallel
        r["_el"] = r["elapsed_ns"] * 1e-9
        r["_mpi"] = sum(f[4] for f in r["mpi"]) * 1e-9
        r["_io"] = io_ns(r) * 1e-9
        r["_comp"] = max(0.0, r["_el"] - r["_mpi"] - r["_io"])
        r["_user"], r["_sys"] = r["cpu_user_us"] * 1e-6, r["cpu_sys_us"] * 1e-6
        cpu = r["_user"] + r["_sys"]
        r["_util"] = 100.0 * cpu / (r["_el"] * max(r["cpu"]["cpus_allowed"], 1)) if r["_el"] > 0 else 0
        r["_utilc"] = (100.0 * cpu / (r["_el"] * max(r["cpu"]["cores_allowed"] or r["cpu"]["cpus_allowed"], 1))
                       if r["_el"] > 0 else 0)
    el, comp = stat(r["_el"] for r in recs), stat(r["_comp"] for r in recs)
    mpi, io = stat(r["_mpi"] for r in recs), stat(r["_io"] for r in recs)

    # MPI by function and class
    funcs = {}
    for r in recs:
        for name, cls, calls, byt, ns in r["mpi"]:
            f = funcs.setdefault(name, {"class": cls, "calls": 0, "bytes": 0, "ns": {}})
            f["calls"] += calls
            f["bytes"] += byt
            f["ns"][r["rank"]] = ns
    by_function = []
    for name, f in sorted(funcs.items(), key=lambda kv: -sum(kv[1]["ns"].values())):
        fs = stat(f["ns"].get(r["rank"], 0) * 1e-9 for r in recs)
        by_function.append({"name": name, "class": f["class"], "calls": f["calls"], "bytes": f["bytes"],
                            "avg_bytes_per_call": f["bytes"] / f["calls"] if f["calls"] else 0.0,
                            "time_s": fs, "pct_of_elapsed": pct(fs["avg"], el["avg"]),
                            "imbalance_pct": imb(fs)})
    by_class = {}
    for c in CLASSES:
        cs = stat(sum(x[4] for x in r["mpi"] if x[1] == c) * 1e-9 for r in recs)
        by_class[c] = {"time_s": cs, "pct_of_elapsed": pct(cs["avg"], el["avg"]),
                       "calls": sum(f["calls"] for f in funcs.values() if f["class"] == c),
                       "bytes": sum(f["bytes"] for f in funcs.values() if f["class"] == c)}
    hist = {c: [[0, 0] for _ in range(34)] for c in CLASSES}
    for r in recs:
        for c, bins in r["hist"].items():
            for b, (calls, ns) in enumerate(bins):
                hist.setdefault(c, [[0, 0] for _ in range(34)])
                hist[c][b][0] += calls
                hist[c][b][1] += ns
    last = max([b for c in hist for b in range(34) if hist[c][b][0]] or [0])
    sizes = {"bin_lower_bytes": [0 if b == 0 else 1 << (b - 1) for b in range(last + 1)]}
    for c in CLASSES:
        if any(hist[c][b][0] for b in range(last + 1)):
            sizes[c] = {"calls": [hist[c][b][0] for b in range(last + 1)],
                        "time_s": [hist[c][b][1] * 1e-9 for b in range(last + 1)]}

    def io_kind(k):
        s = lambda key: sum(r["io"][k][key] for r in recs)
        return {"calls": {"open": s("open_calls"), "close": s("close_calls"), "read": s("read_calls"),
                          "write": s("write_calls"), "sync": s("sync_calls")},
                "bytes": {"read": s("read_bytes"), "write": s("write_bytes")},
                "time_s": {t: stat(r["io"][k][t + "_ns"] * 1e-9 for r in recs) for t in ("read", "write", "meta")}}

    parallel = {
        "definitions": {"compute_s": "elapsed - MPI time - I/O time, per rank",
                        "imbalance_pct": "(max - avg) / max over the ranks that wrote a record",
                        "partial": "from the per-rank records of a run that ended without MPI_Finalize"},
        "time_split_pct": {"compute": pct(comp["avg"], el["avg"]), "mpi": pct(mpi["avg"], el["avg"]),
                           "io": pct(io["avg"], el["avg"])},
        "compute": {"time_s": comp, "imbalance_pct": imb(comp),
                    "imbalance_cost_pct": pct(comp["max"] - comp["avg"], el["max"]),
                    "slowest_ranks": [{"rank": r["rank"], "compute_s": r["_comp"]}
                                      for r in sorted(recs, key=lambda r: -r["_comp"])[:5]]},
        "mpi": {"time_s": mpi, "pct_of_elapsed": pct(mpi["avg"], el["avg"]), "imbalance_pct": imb(mpi),
                "by_class": by_class, "by_function": by_function, "message_sizes": sizes},
        "io": {"time_s": io, "pct_of_elapsed": pct(io["avg"], el["avg"]),
               "posix": io_kind("posix"), "mpiio": io_kind("mpiio")},
        "cpu": {"utilisation_pct": stat(r["_util"] for r in recs),
                "utilisation_per_core_pct": stat(r["_utilc"] for r in recs),
                "user_s": stat(r["_user"] for r in recs), "sys_s": stat(r["_sys"] for r in recs),
                "max_rss_mb": stat(r["max_rss_kb"] / 1024.0 for r in recs)},
    }

    # hardware counters, where every record has the same group
    with_cnt = [r for r in recs if (r.get("counters") or {}).get("events")]
    counters = {"available": False, "reason": "no counter values in the partial records "
                "(not measured, or the top-down level, whose event sets rotate over threads)"}
    if with_cnt and len({r["counters"]["group"] for r in with_cnt}) == 1:
        keys = list(with_cnt[0]["counters"]["events"])
        cp = {k: sum(r["counters"]["events"].get(k, [0, 0])[0] for r in with_cnt) for k in keys}
        mp = {k: sum(r["counters"]["events"].get(k, [0, 0])[1] for r in with_cnt) for k in keys}
        tot = {k: cp[k] + mp[k] for k in keys}
        der = {}
        if "cycles" in tot and "instructions" in tot:
            der.update(ipc=ratio(tot["instructions"], tot["cycles"]),
                       ipc_compute=ratio(cp["instructions"], cp["cycles"]),
                       ipc_mpi=ratio(mp["instructions"], mp["cycles"]))
            ut = sum(r["_user"] for r in with_cnt)
            der["ghz_effective"] = tot["cycles"] / ut * 1e-9 if ut > 0 else 0.0
        if "flops" in tot:
            der["gflops_per_s"] = tot["flops"] / el["max"] * 1e-9 if el["max"] > 0 else 0.0
            cs = sum(r["_comp"] for r in with_cnt)
            der["gflops_per_s_per_rank_in_compute"] = cp["flops"] / cs * 1e-9 if cs > 0 else 0.0
            if "cycles" in tot:
                der["flops_per_cycle"] = ratio(tot["flops"], tot["cycles"])
        counters = {"available": True, "uarch": with_cnt[0]["cpu"]["uarch"] or "unknown",
                    "group": with_cnt[0]["counters"]["group"], "read_method": "partial record",
                    "ranks_without_counters": n - len(with_cnt), "multiplexed_ranks": 0,
                    "partial_note": "counts up to each thread's last MPI call or OpenMP transition",
                    "totals": tot, "compute": cp, "mpi": mp, "derived": der, "regions": []}

    # nodes by hostname, in rank order
    hosts = []
    for r in recs:
        if r["hostname"] not in hosts:
            hosts.append(r["hostname"])
    nodes = [{"index": i, "hostname": h, "ranks": sum(r["hostname"] == h for r in recs),
              "cpu": next(dict(r["cpu"]) for r in recs if r["hostname"] == h)} for i, h in enumerate(hosts)]
    ranks = [{"rank": r["rank"], "node": hosts.index(r["hostname"]), "elapsed_s": r["_el"],
              "compute_s": r["_comp"], "mpi_s": r["_mpi"], "io_s": r["_io"],
              "cpu_util_pct": r["_util"], "cpu_util_per_core_pct": r["_utilc"]} for r in recs]

    reasons = {}
    for r in recs:
        reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1
    seen = {r["rank"] for r in recs}
    first = recs[0]
    cmd = next((r["command"] for r in recs if r["command"]), "")
    collected = ["parallel"] + (["counters"] if counters["available"] else [])
    return {
        "schema": "miniprof-snapshot", "schema_version": "0.9.0",
        "tool": dict(first["tool"], name="miniprof", merged_by="miniprof_partial.py"),
        "run": {"start_utc": first["start_utc"], "command": cmd, "level": first["level"],
                "nranks": nranks, "nnodes": len(hosts), "elapsed_s": el,
                "partial": {"reasons": reasons, "ranks_reported": n, "ranks_missing": nranks - n,
                            "missing": sorted(set(range(nranks)) - seen)[:64],
                            "unreadable_records": bad,
                            "not_available": ["topdown", "memory", "roofline", "openmp",
                                              "hotspots", "timeline", "regions"]}},
        "nodes": nodes, "ranks": ranks,
        "metrics": {"collected": collected, "parallel": parallel, "counters": counters,
                    "topdown": None, "memory": None, "roofline": None, "openmp": None,
                    "hotspots": None, "timeline": None},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", help="the <snapshot>.partial directory")
    ap.add_argument("-o", "--output", help="snapshot to write (default: the directory name without .partial)")
    a = ap.parse_args()
    recs, bad = load(a.dir)
    if not recs:
        sys.exit(f"miniprof_partial: no rank records in {a.dir}")
    doc = merge(recs, bad)
    out = a.output or (a.dir[:-len(".partial")] if a.dir.rstrip("/").endswith(".partial")
                       else a.dir.rstrip("/") + ".json")
    out = out.rstrip("/")
    with open(out, "w") as f:
        json.dump(doc, f, indent=1)
    p = doc["run"]["partial"]
    print(f"miniprof_partial: {p['ranks_reported']} of {doc['run']['nranks']} ranks "
          f"({', '.join(f'{k}: {v}' for k, v in p['reasons'].items())}) -> {out}")


main()
