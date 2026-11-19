#!/usr/bin/env python3
"""validate_partial.py — runs that never reach MPI_Finalize leave a partial
snapshot with the right counts (examples/partial_validate).

    validate_partial.py [--mpirun "mpirun --oversubscribe"] [--ranks N] PROGRAM

Runs PROGRAM four ways under MINIPROF_MODE=snapshot, merges the per-rank
records with tools/miniprof_partial.py and checks the result:

  term     SIGTERM to mpirun once every rank is computing (a batch time limit)
  abort    the last rank calls MPI_Abort
  exit     every rank calls exit(3) without MPI_Finalize
  handler  as term, with the program's own SIGTERM handler: it must still run
           (after miniprof's record), and no rank may be left without one

Every rank made 20 MPI_Allreduce of 8 doubles, 3 MPI_Barrier and one
4096-byte write before the end, so those totals are exact for the ranks that
left a record."""
import argparse, json, os, shlex, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
K, BARRIERS, WRITE = 20, 3, 4096
fails = 0


def check(ok, msg):
    global fails
    print(("  ok    " if ok else "  FAIL  ") + msg)
    fails += 0 if ok else 1


def run(mode, args, prog, work):
    out = os.path.join(work, mode)
    os.makedirs(out)
    snap = os.path.join(out, "s.json")
    env = dict(os.environ, MINIPROF_MODE="snapshot", MINIPROF_SNAPSHOT_OUTPUT=snap)
    cmd = shlex.split(args.mpirun) + ["-n", str(args.ranks), "-x", "MINIPROF_MODE",
                                      "-x", "MINIPROF_SNAPSHOT_OUTPUT", prog, mode, out]
    log = open(os.path.join(out, "out.txt"), "w+")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    if mode in ("term", "handler"):
        t_end = time.time() + 60
        while time.time() < t_end and p.poll() is None:
            log.seek(0)
            if log.read().count("ready") >= args.ranks:
                break
            time.sleep(0.1)
        time.sleep(0.3)
        p.send_signal(signal.SIGTERM)
    try:
        rc = p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = p.wait()
    log.seek(0)
    text = log.read()
    part = snap + ".partial"
    m = subprocess.run([sys.executable, os.path.join(HERE, "miniprof_partial.py"), part],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    doc = json.load(open(snap)) if m.returncode == 0 and os.path.exists(snap) else None
    return rc, text, doc, m.stdout.strip()


def counts_ok(doc, n_rep, label):
    fn = {f["name"]: f for f in doc["metrics"]["parallel"]["mpi"]["by_function"]}
    ar, ba = fn.get("MPI_Allreduce") or {}, fn.get("MPI_Barrier") or {}
    check(ar.get("calls") == K * n_rep and ar.get("bytes") == K * 64 * n_rep,
          f"{label}: MPI_Allreduce {ar.get('calls')} calls, {ar.get('bytes')} B "
          f"(= {K} x 64 B x {n_rep} ranks)")
    check(ba.get("calls") == BARRIERS * n_rep, f"{label}: MPI_Barrier {ba.get('calls')} calls")
    px = doc["metrics"]["parallel"]["io"]["posix"]
    check(px["bytes"]["write"] == WRITE * n_rep and px["calls"]["write"] == n_rep,
          f"{label}: POSIX write {px['bytes']['write']} B in {px['calls']['write']} calls")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mpirun", default="mpirun")
    ap.add_argument("--ranks", type=int, default=2)
    ap.add_argument("program")
    args = ap.parse_args()
    prog = os.path.abspath(args.program)
    n = args.ranks
    with tempfile.TemporaryDirectory(prefix="miniprof_partial_") as work:
        for mode in ("term", "abort", "exit", "handler"):
            rc, text, doc, merged = run(mode, args, prog, work)
            print(f"{mode}: mpirun exit {rc}; {merged}")
            if not doc:
                check(False, f"{mode}: no partial snapshot ({text[-300:]!r})")
                continue
            pt = doc["run"]["partial"]
            reasons, rep = pt["reasons"], pt["ranks_reported"]
            check(doc["run"]["nranks"] == n, f"{mode}: run of {doc['run']['nranks']} ranks")
            if mode == "term":
                check(reasons == {"SIGTERM": n}, f"{mode}: every rank recorded SIGTERM ({reasons})")
            elif mode == "abort":
                check(reasons.get("MPI_Abort") == 1, f"{mode}: the aborting rank recorded MPI_Abort ({reasons})")
            elif mode == "exit":
                check(reasons == {"exit without MPI_Finalize": n}, f"{mode}: every rank recorded exit ({reasons})")
                check(rc == 3, f"{mode}: the exit code (3) stands ({rc})")
            else:
                check(reasons == {"SIGTERM": n}, f"{mode}: every rank recorded SIGTERM ({reasons})")
                ran = text.count("application handler ran")
                check(ran == n, f"{mode}: the program's own handler ran on every rank ({ran}), after the record")
            counts_ok(doc, rep, mode)
            r = subprocess.run([sys.executable, os.path.join(HERE, "miniprof_report.py"),
                                os.path.join(work, mode, "s.json")], stdout=subprocess.PIPE,
                               universal_newlines=True)
            check(r.returncode == 0 and "PARTIAL" in r.stdout, f"{mode}: the report says PARTIAL")
    print("PASS" if not fails else f"FAIL ({fails} checks)")
    sys.exit(1 if fails else 0)


main()
