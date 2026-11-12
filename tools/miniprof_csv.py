"""
miniprof_csv.py — a snapshot's tables as CSV files, for spreadsheets and
pandas. Used by miniprof_report.py --csv DIR.

One file per table, one row per entity, nested values flattened into
dotted column names (time_s.avg, level1.retiring). Only the tables the
snapshot has are written. Python standard library only.

  summary.csv           one row: run, time split, headline counters, memory, roofline
  ranks.csv             per rank: time split, CPU utilisation, placement
  mpi_functions.csv     per MPI function: class, calls, bytes, time, imbalance
  message_sizes.csv     per MPI class and size bin: calls, seconds
  io.csv                per interface (POSIX, MPI-IO): bytes, calls, time
  regions.csv           per user region: time, counters, derived, top-down, roofline
  openmp_regions.csv    per OpenMP parallel region
  memory_nodes.csv      per node: read/write/peak GB/s, power
  memory_sockets.csv    per node and socket
  memory_channels.csv   per node, socket and channel: read/write GB/s
  roofline_nodes.csv    per node: GFLOP/s, DRAM GB/s, intensity, bound, efficiencies
"""

import csv
import os


def flat(d, prefix="", out=None):
    """{'a': {'b': 1}, 'c': [1, 2]} -> {'a.b': 1, 'c': '1;2'}"""
    out = {} if out is None else out
    for k, v in (d or {}).items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            flat(v, key + ".", out)
        elif isinstance(v, list):
            if all(not isinstance(x, (dict, list)) for x in v):
                out[key] = ";".join("" if x is None else str(x) for x in v)
            # lists of objects are tables of their own, not columns
        else:
            out[key] = v
    return out


def write_table(path, rows, first=()):
    """rows: list of flat dicts; columns = `first`, then the rest in first-seen order."""
    cols = [c for c in first if any(c in r for r in rows)]
    for r in rows:
        for k in r:
            if k not in cols:
                cols.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in cols})


def write_csv(doc: dict, outdir: str) -> list:
    os.makedirs(outdir, exist_ok=True)
    run = doc.get("run") or {}
    nodes = doc.get("nodes") or []
    m = doc.get("metrics") or {}
    host = {n.get("index", i): n.get("hostname") for i, n in enumerate(nodes)}
    written = []

    def table(name, rows, first=()):
        if rows:
            p = os.path.join(outdir, name)
            write_table(p, rows, first)
            written.append(p)

    # ---- summary: one wide row
    s = {"command": run.get("command"), "nranks": run.get("nranks"), "nnodes": run.get("nnodes"),
         "omp_num_threads": run.get("omp_num_threads"), "level": run.get("level"),
         "start_utc": run.get("start_utc"), "elapsed_s.max": (run.get("elapsed_s") or {}).get("max")}
    if nodes:
        s["cpu"] = nodes[0]["cpu"].get("brand")
        s["uarch"] = nodes[0]["cpu"].get("uarch")
    par = m.get("parallel")
    if par:
        flat({"time_split_pct": par.get("time_split_pct")}, "", s)
        s["compute.imbalance_pct"] = par["compute"].get("imbalance_pct")
        s["compute.imbalance_cost_pct"] = par["compute"].get("imbalance_cost_pct")
        s["mpi.imbalance_pct"] = par["mpi"].get("imbalance_pct")
        cu = par["cpu"].get("utilisation_per_core_pct") or par["cpu"].get("utilisation_pct") or {}
        s["cpu_util_pct.avg"] = cu.get("avg")
    cnt = m.get("counters") or {}
    if cnt.get("available"):
        s["counters.group"] = cnt.get("group")
        flat(cnt.get("derived"), "counters.", s)
    td = m.get("topdown") or {}
    if td.get("available"):
        flat((td.get("compute") or {}), "topdown.", s)
    mem = m.get("memory") or {}
    if mem.get("available"):
        flat(mem.get("per_node"), "memory.per_node.", s)
        s["memory.remote_read_share_pct"] = mem.get("remote_read_share_pct")
        s["memory.coverage"] = mem.get("coverage")
    omp = m.get("openmp") or {}
    if omp.get("available"):
        for k in ("serial_pct_of_elapsed", "imbalance_pct", "lock_wait_pct", "threads"):
            s[f"openmp.{k}"] = omp.get(k)
    roof = m.get("roofline") or {}
    if roof.get("available") and roof.get("nodes"):
        rn = roof["nodes"][0]
        s["roofline.estimate"] = roof.get("estimate")
        for k in ("gflops_per_s", "dram_gb_s", "arithmetic_intensity", "bound", "pct_of_attainable",
                  "pct_of_peak_gflops", "pct_of_dram_bw"):
            s[f"roofline.node0.{k}"] = rn.get(k)
    table("summary.csv", [s])

    # ---- per rank
    ranks = sorted(doc.get("ranks") or [], key=lambda r: r["rank"])
    table("ranks.csv", [dict(flat(r), hostname=host.get(r.get("node"))) for r in ranks],
          first=("rank", "node", "hostname", "elapsed_s", "compute_s", "mpi_s", "io_s"))

    if par:
        table("mpi_functions.csv", [flat(f) for f in par["mpi"].get("by_function") or []],
              first=("name", "class", "calls", "bytes", "pct_of_elapsed", "imbalance_pct"))
        ms = par["mpi"].get("message_sizes") or {}
        lo = ms.get("bin_lower_bytes") or []
        rows = []
        for cls, h in ms.items():
            if cls == "bin_lower_bytes":
                continue
            for i, b in enumerate(lo):
                c = (h.get("calls") or [0] * len(lo))[i]
                t = (h.get("time_s") or [0] * len(lo))[i]
                if c or t:
                    rows.append({"class": cls, "bin_lower_bytes": b,
                                 "bin_upper_bytes": lo[i + 1] if i + 1 < len(lo) else "", "calls": c, "time_s": t})
        table("message_sizes.csv", rows)
        io = par.get("io") or {}
        table("io.csv", [dict(flat(io[k]), interface=k) for k in ("posix", "mpiio") if k in io],
              first=("interface",))

    # ---- regions: one row per name, joining counters, top-down and roofline
    regs = {}
    for r in cnt.get("regions") or []:
        regs.setdefault(r["name"], {"name": r["name"]}).update(flat(r))
    for r in (td.get("regions") or []) if td.get("available") else []:
        e = regs.setdefault(r["name"], {"name": r["name"]})
        flat(r.get("topdown"), "topdown.", e)
    for r in (roof.get("regions") or []) if roof.get("available") else []:
        e = regs.setdefault(r["name"], {"name": r["name"]})
        flat({k: v for k, v in r.items() if k != "name"}, "roofline.", e)
    table("regions.csv", list(regs.values()), first=("name", "ranks", "calls", "time_s.avg", "pct_of_elapsed"))

    if omp.get("available"):
        table("openmp_regions.csv", [flat(r) for r in omp.get("regions") or []],
              first=("name", "location", "calls", "wall_s_per_rank", "imbalance_pct"))

    # ---- memory
    if mem.get("available"):
        nrows, srows, crows = [], [], []
        for nd in mem.get("nodes") or []:
            nrows.append(flat({k: v for k, v in nd.items() if k != "sockets"}))
            for so in nd.get("sockets") or []:
                ch = so.get("channels") or {}
                srows.append(dict(flat({k: v for k, v in so.items() if k != "channels"}),
                                  node=nd.get("index"), hostname=nd.get("hostname"),
                                  channels_count=ch.get("count"), channels_active=ch.get("active")))
                rd, wr = ch.get("read_gb_s") or [], ch.get("write_gb_s") or []
                for i in range(max(len(rd), len(wr))):
                    crows.append({"node": nd.get("index"), "hostname": nd.get("hostname"), "socket": so.get("socket"),
                                  "channel": i, "read_gb_s": rd[i] if i < len(rd) else "",
                                  "write_gb_s": wr[i] if i < len(wr) else ""})
        table("memory_nodes.csv", nrows, first=("index", "hostname"))
        table("memory_sockets.csv", srows, first=("node", "hostname", "socket"))
        table("memory_channels.csv", crows)

    if roof.get("available"):
        table("roofline_nodes.csv", [flat(n) for n in roof.get("nodes") or []], first=("index", "hostname"))
    return written
