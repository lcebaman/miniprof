#!/usr/bin/env python3
"""validate_mpi_coverage.py — the snapshot of examples/mpi_coverage_validate
(C or Fortran) against the exact call counts and bytes that program makes.

    validate_mpi_coverage.py SNAPSHOT.json

Every rank makes the same calls with fixed sizes, so each function's total
over ranks is a known function of the rank count n. Checks every function
the program calls, the classes they land in, and that "comm" (communicator
management) is reported as its own class."""
import json, sys

NS, NB, K_ALL, K_ONE = 10, 100, 5, 3
D, I = 8, 4                      # sizeof(double), sizeof(int)


def expected(n):
    """per-rank (calls, bytes, class) for every function the program calls"""
    e = {
        # persistent: bytes at each start, none at *_init
        "MPI_Send_init": (1, 0, "p2p"),
        "MPI_Recv_init": (1, 0, "p2p"),
        "MPI_Startall": (K_ALL, K_ALL * 2 * NS * D, "p2p"),
        "MPI_Start": (2 * K_ONE, 2 * K_ONE * NS * D, "p2p"),
        # matched probe (MPI_Improbe: polled until found, checked separately)
        "MPI_Mprobe": (1, 0, "wait"),
        "MPI_Mrecv": (1, 4 * I, "p2p"),
        "MPI_Imrecv": (1, 4 * I, "p2p"),
        # nonblocking collectives: same byte rules as the blocking forms
        "MPI_Ibarrier": (1, 0, "collective"),
        "MPI_Ibcast": (1, NB * D, "collective"),
        "MPI_Ireduce": (1, NB * D, "collective"),
        "MPI_Iallreduce": (1, NB * D, "collective"),
        "MPI_Igather": (1, NS * D, "collective"),
        "MPI_Igatherv": (1, NS * D, "collective"),
        "MPI_Iallgather": (1, NS * D, "collective"),
        "MPI_Iallgatherv": (1, NS * D, "collective"),
        "MPI_Iscatter": (1, NS * D, "collective"),
        "MPI_Iscatterv": (1, NS * D, "collective"),
        "MPI_Ialltoall": (1, n * NS * D, "collective"),
        "MPI_Ialltoallv": (1, n * NS * D, "collective"),
        "MPI_Ireduce_scatter": (1, n * NS * D, "collective"),
        "MPI_Ireduce_scatter_block": (1, n * NS * D, "collective"),
        "MPI_Iscan": (1, NB * D, "collective"),
        "MPI_Iexscan": (1, NB * D, "collective"),
        # neighbourhood collectives on a periodic ring: 2 in-neighbours
        "MPI_Neighbor_allgather": (1, 2 * NS * D, "collective"),
        "MPI_Neighbor_allgatherv": (1, 2 * NS * D, "collective"),
        "MPI_Neighbor_alltoall": (1, 2 * NS * D, "collective"),
        "MPI_Neighbor_alltoallv": (1, 2 * NS * D, "collective"),
        "MPI_Ineighbor_allgather": (1, 2 * NS * D, "collective"),
        "MPI_Ineighbor_allgatherv": (1, 2 * NS * D, "collective"),
        "MPI_Ineighbor_alltoall": (1, 2 * NS * D, "collective"),
        "MPI_Ineighbor_alltoallv": (1, 2 * NS * D, "collective"),
        # communicators and topologies: time only
        "MPI_Comm_dup": (1, 0, "comm"),
        "MPI_Comm_split": (1, 0, "comm"),
        "MPI_Comm_split_type": (1, 0, "comm"),
        "MPI_Comm_create": (1, 0, "comm"),
        "MPI_Comm_create_group": (1, 0, "comm"),
        "MPI_Cart_create": (2, 0, "comm"),
        "MPI_Cart_sub": (1, 0, "comm"),
        "MPI_Dist_graph_create": (1, 0, "comm"),
        "MPI_Dist_graph_create_adjacent": (1, 0, "comm"),
        "MPI_Intercomm_create": (1, 0, "comm"),
        "MPI_Intercomm_merge": (1, 0, "comm"),
        "MPI_Comm_free": (12, 0, "comm"),
        # one-sided
        "MPI_Win_allocate": (1, 0, "rma"),
        "MPI_Win_allocate_shared": (1, 0, "rma"),
        "MPI_Win_create_dynamic": (1, 0, "rma"),
        "MPI_Win_lock": (1, 0, "rma"),
        "MPI_Win_unlock": (1, 0, "rma"),
        "MPI_Win_lock_all": (1, 0, "rma"),
        "MPI_Win_unlock_all": (1, 0, "rma"),
        "MPI_Win_flush": (1, 0, "rma"),
        "MPI_Win_flush_all": (1, 0, "rma"),
        "MPI_Win_flush_local": (1, 0, "rma"),
        "MPI_Win_flush_local_all": (1, 0, "rma"),
        "MPI_Win_sync": (1, 0, "rma"),
        "MPI_Win_post": (1, 0, "rma"),
        "MPI_Win_start": (1, 0, "rma"),
        "MPI_Win_complete": (1, 0, "rma"),
        "MPI_Win_wait": (1, 0, "rma"),
        "MPI_Put": (2, 2 * I, "rma"),
        "MPI_Fetch_and_op": (1, I, "rma"),
        "MPI_Compare_and_swap": (1, I, "rma"),
        "MPI_Get_accumulate": (1, I, "rma"),
        "MPI_Rput": (1, I, "rma"),
        "MPI_Rget": (1, I, "rma"),
        "MPI_Raccumulate": (1, I, "rma"),
        "MPI_Win_free": (3, 0, "rma"),
    }
    return e


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = json.load(open(sys.argv[1]))
    n = len(d.get("ranks") or [])
    mpi = ((d.get("metrics") or {}).get("parallel") or {}).get("mpi") or {}
    fn = {f["name"]: f for f in mpi.get("by_function") or []}
    print(f"mpi_coverage_validate: {n} ranks, {len(fn)} MPI functions in the snapshot")
    fails = 0
    for name, (calls, nbytes, cls) in expected(n).items():
        f = fn.get(name)
        want = (calls * n, nbytes * n, cls)
        got = (f["calls"], f["bytes"], f["class"]) if f else None
        ok = got == want
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'}  {name:32s} calls {got[0] if got else '-':>4} "
              f"bytes {got[1] if got else '-':>7} {got[2] if got else '':10s}"
              + ("" if ok else f"   expected calls {want[0]} bytes {want[1]} {want[2]}"))
    imp = fn.get("MPI_Improbe")
    ok = bool(imp) and imp["calls"] >= n and imp["bytes"] == 0 and imp["class"] == "wait"
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'}  MPI_Improbe                      polled: "
          f"{imp['calls'] if imp else 0} calls >= {n}")
    comm = (mpi.get("by_class") or {}).get("comm")
    ok = bool(comm) and comm.get("calls") == sum(c for c, _, k in expected(n).values() if k == "comm") * n
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'}  class \"comm\" reported with every communicator call")
    # MPI-IO: only the mpi_f08 program does any (open, write_at_all,
    # sync, read_at, close: 100 doubles each way per rank)
    io = (((d.get("metrics") or {}).get("parallel") or {}).get("io") or {}).get("mpiio") or {}
    calls, byt = io.get("calls") or {}, io.get("bytes") or {}
    if any(calls.values()):
        want = ({"open": n, "close": n, "read": n, "write": n, "sync": n},
                {"read": n * NB * D, "write": n * NB * D})
        ok = (calls, byt) == want
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'}  MPI-IO                           calls {calls} bytes {byt}"
              + ("" if ok else f"   expected {want}"))
    print("PASS" if not fails else f"FAIL ({fails} checks)")
    sys.exit(1 if fails else 0)


main()
