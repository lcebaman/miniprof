#!/usr/bin/env python3
"""check_exports.py — libminiprof.so exports its wrappers and public API,
and nothing else (src/libminiprof.map).

    check_exports.py libminiprof.so src/libminiprof.map OBJECT.o...

The library is preloaded, so every exported symbol takes precedence over
the application's own. Two ways to get that wrong, both checked:

- a helper leaks: an exported symbol that is not a wrapper or public API
  could replace an application function of the same name;
- a wrapper is hidden: a global function in the objects that the map does
  not export intercepts nothing. Internal functions carry a module prefix
  (cnt_, tl_, jw_ ...); a hidden global without one is most likely a new
  interposer missing from the map."""
import re, subprocess, sys

# public patterns besides the explicit names in the map
PUBLIC = [re.compile(p) for p in (r"^MPI_[A-Z][A-Za-z0-9_]*$", r"^mpi_[a-z0-9_]+_$",
                                  r"^miniprof_[a-z0-9_]+$")]
INTERNAL_PREFIX = ("aggregator_", "cnt_", "hotspots_", "hwc_", "instrument_", "io_trace_",
                   "jw_", "mp_p", "mpa_", "mpi_trace_", "nodemon_", "ompt_collect",
                   "ompt_snapshot_", "path_store_", "profile_store_", "sampler_", "snapshot_",
                   "symbolizer_", "sysinfo_", "tl_", "trace_store_", "trace_aggregator_",
                   "g_miniprof_")
MUST_EXPORT = ("MPI_Init", "MPI_Finalize", "MPI_Send", "MPI_Allreduce", "MPI_Start",
               "mpi_init_", "mpi_send_", "mpi_init_f08_", "mpi_send_f08_", "mpi_send_f08ts_",
               "mpi_file_open_f08_", "open", "read", "write", "fopen", "__read_chk",
               "pthread_create", "ompt_start_tool", "miniprof_region_begin",
               "miniprof_region_begin_", "__cyg_profile_func_enter")


def syms(args):
    out = subprocess.run(["nm"] + args, stdout=subprocess.PIPE, universal_newlines=True,
                         check=True).stdout
    return {ln.split()[-1] for ln in out.splitlines()
            if len(ln.split()) >= 2 and ln.split()[-2] in "TWDBRVi"}


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    so, mapf, objs = sys.argv[1], sys.argv[2], sys.argv[3:]
    text = re.sub(r"/\*.*?\*/", "", open(mapf).read(), flags=re.S)
    glob = text.split("global:")[1].split("local:")[0]
    explicit = {n.strip() for n in glob.split(";") if n.strip() and "*" not in n}
    public = lambda s: s in explicit or any(p.match(s) for p in PUBLIC)

    exported = syms(["-D", "--defined-only", so]) - {"_init", "_fini"}
    defined = set()
    for o in objs:
        defined |= syms(["-g", "--defined-only", o])

    leaks = sorted(s for s in exported if not public(s))
    hidden = sorted(s for s in defined - exported
                    if not s.startswith(INTERNAL_PREFIX) and s not in ("main",))
    missing = sorted(s for s in MUST_EXPORT if s not in exported)
    absent = sorted(s for s in explicit if s not in exported)

    ok = True
    for what, lst in (("exported but not a wrapper or public API (add a module prefix and keep it local)", leaks),
                      ("global in the objects but hidden, without an internal prefix (an interposer missing from the map?)", hidden),
                      ("must be exported but is not", missing),
                      ("listed in the map but not defined", absent)):
        if lst:
            ok = False
            print(f"FAIL: {what}: {' '.join(lst)}")
    print(f"{'PASS' if ok else 'FAIL'}: {len(exported)} symbols exported, "
          f"{len(defined - exported)} kept local")
    sys.exit(0 if ok else 1)


main()
