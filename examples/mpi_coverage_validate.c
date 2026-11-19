/*
 * mpi_coverage_validate.c — known answers for the MPI wrappers added after
 * the audit: persistent requests, matched probe, nonblocking and
 * neighbourhood collectives, communicator and topology management, and the
 * rest of one-sided (allocated windows, passive target, PSCW, atomics,
 * request-based RMA).
 *
 * Every rank makes each call a fixed number of times with fixed sizes, so
 * tools/validate_mpi_coverage.py can compute every function's exact call
 * count and byte total for the run's rank count, and check the snapshot
 * against them. The program also checks its own results (wrong data
 * aborts), so a wrapper that mangles an argument fails here too.
 * mpi_coverage_validate.f90 makes the same calls through the Fortran
 * bindings, with the same expected totals.
 *
 * Needs >= 2 ranks.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define NS      10      /* doubles per small message: 80 B   */
#define NB      100     /* doubles per larger message: 800 B */
#define K_ALL   5       /* MPI_Startall rounds */
#define K_ONE   3       /* MPI_Start rounds (each request)  */

static int g_rank, g_size;

static void fail(const char *what, double got, double want)
{
    fprintf(stderr, "rank %d: %s: got %g, expected %g\n", g_rank, what, got, want);
    MPI_Abort(MPI_COMM_WORLD, 1);
}
static void expect(const char *what, double got, double want)
{
    if (got != want) fail(what, got, want);
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &g_size);
    if (g_size < 2) {
        if (g_rank == 0) fprintf(stderr, "mpi_coverage_validate: needs >= 2 ranks\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    const int n = g_size, me = g_rank;
    const int right = (me + 1) % n, left = (me - 1 + n) % n;
    double s[NB * 4], r[NB * 4];
    MPI_Request q[8];

    /* ── persistent: Send_init/Recv_init, Startall x5, Start x3 each ── */
    {
        MPI_Request pr[2];
        MPI_Send_init(s, NS, MPI_DOUBLE, right, 1, MPI_COMM_WORLD, &pr[0]);
        MPI_Recv_init(r, NS, MPI_DOUBLE, left, 1, MPI_COMM_WORLD, &pr[1]);
        for (int k = 0; k < K_ALL; k++) {
            for (int i = 0; i < NS; i++) s[i] = me * 100 + k;
            MPI_Startall(2, pr);
            MPI_Waitall(2, pr, MPI_STATUSES_IGNORE);
            expect("persistent Startall data", r[0], left * 100 + k);
        }
        for (int k = 0; k < K_ONE; k++) {
            MPI_Start(&pr[1]);
            MPI_Start(&pr[0]);
            MPI_Waitall(2, pr, MPI_STATUSES_IGNORE);
        }
        MPI_Request_free(&pr[0]);
        MPI_Request_free(&pr[1]);
    }

    /* ── matched probe: Mprobe+Mrecv, Improbe (until found)+Imrecv ──── */
    {
        int v[4] = { me, me, me, me }, w[4];
        MPI_Message m;
        MPI_Isend(v, 4, MPI_INT, right, 2, MPI_COMM_WORLD, &q[0]);
        MPI_Mprobe(left, 2, MPI_COMM_WORLD, &m, MPI_STATUS_IGNORE);
        MPI_Mrecv(w, 4, MPI_INT, &m, MPI_STATUS_IGNORE);
        MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        expect("Mrecv data", w[3], left);

        MPI_Isend(v, 4, MPI_INT, right, 3, MPI_COMM_WORLD, &q[0]);
        int flag = 0;
        while (!flag) MPI_Improbe(left, 3, MPI_COMM_WORLD, &flag, &m, MPI_STATUS_IGNORE);
        MPI_Imrecv(w, 4, MPI_INT, &m, &q[1]);
        MPI_Waitall(2, q, MPI_STATUSES_IGNORE);
        expect("Imrecv data", w[0], left);
    }

    /* ── nonblocking collectives, one each, then Wait ────────────────── */
    {
        int cnt[64], dsp[64];
        if (n > 64) { if (me == 0) fprintf(stderr, "use <= 64 ranks\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
        for (int i = 0; i < n; i++) { cnt[i] = NS; dsp[i] = i * NS; }
        double *big_s = calloc((size_t)n * NS, sizeof(double));
        double *big_r = calloc((size_t)n * NS, sizeof(double));
        for (int i = 0; i < NB; i++) s[i] = 1.0;

        MPI_Ibarrier(MPI_COMM_WORLD, &q[0]);                                   MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ibcast(s, NB, MPI_DOUBLE, 0, MPI_COMM_WORLD, &q[0]);               MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ireduce(s, r, NB, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD, &q[0]);  MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        if (me == 0) expect("Ireduce sum", r[0], n);
        MPI_Iallreduce(s, r, NB, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD, &q[0]);  MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        expect("Iallreduce sum", r[NB - 1], n);
        MPI_Igather(s, NS, MPI_DOUBLE, big_r, NS, MPI_DOUBLE, 0, MPI_COMM_WORLD, &q[0]);           MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Igatherv(s, NS, MPI_DOUBLE, big_r, cnt, dsp, MPI_DOUBLE, 0, MPI_COMM_WORLD, &q[0]);   MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Iallgather(s, NS, MPI_DOUBLE, big_r, NS, MPI_DOUBLE, MPI_COMM_WORLD, &q[0]);          MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Iallgatherv(s, NS, MPI_DOUBLE, big_r, cnt, dsp, MPI_DOUBLE, MPI_COMM_WORLD, &q[0]);  MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Iscatter(big_s, NS, MPI_DOUBLE, r, NS, MPI_DOUBLE, 0, MPI_COMM_WORLD, &q[0]);         MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Iscatterv(big_s, cnt, dsp, MPI_DOUBLE, r, NS, MPI_DOUBLE, 0, MPI_COMM_WORLD, &q[0]); MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ialltoall(big_s, NS, MPI_DOUBLE, big_r, NS, MPI_DOUBLE, MPI_COMM_WORLD, &q[0]);       MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ialltoallv(big_s, cnt, dsp, MPI_DOUBLE, big_r, cnt, dsp, MPI_DOUBLE, MPI_COMM_WORLD, &q[0]); MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        for (int i = 0; i < n * NS; i++) big_s[i] = 1.0;
        MPI_Ireduce_scatter(big_s, r, cnt, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD, &q[0]);           MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        expect("Ireduce_scatter sum", r[0], n);
        MPI_Ireduce_scatter_block(big_s, r, NS, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD, &q[0]);      MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Iscan(s, r, NB, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD, &q[0]);                          MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        expect("Iscan prefix", r[0], me + 1);
        MPI_Iexscan(s, r, NB, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD, &q[0]);                        MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        if (me > 0) expect("Iexscan prefix", r[0], me);
        free(big_s);
        free(big_r);
    }

    /* ── communicators and topologies ────────────────────────────────── */
    MPI_Comm dup, split, shm, created, cgroup, ring, grid, sub, dg, dga, inter, merged, half;
    MPI_Group wgroup;
    MPI_Comm_group(MPI_COMM_WORLD, &wgroup);
    MPI_Comm_dup(MPI_COMM_WORLD, &dup);
    MPI_Comm_split(MPI_COMM_WORLD, me % 2, me, &split);
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, me, MPI_INFO_NULL, &shm);
    MPI_Comm_create(MPI_COMM_WORLD, wgroup, &created);
    MPI_Comm_create_group(MPI_COMM_WORLD, wgroup, 7, &cgroup);
    {
        int dims1[1] = { n }, per1[1] = { 1 };
        MPI_Cart_create(MPI_COMM_WORLD, 1, dims1, per1, 0, &ring);
        int dims2[2] = { n, 1 }, per2[2] = { 0, 0 }, keep[2] = { 1, 0 };
        MPI_Cart_create(MPI_COMM_WORLD, 2, dims2, per2, 0, &grid);
        MPI_Cart_sub(grid, keep, &sub);
        int srcs[1] = { me }, deg[1] = { 1 }, dst[1] = { right };
        MPI_Dist_graph_create(MPI_COMM_WORLD, 1, srcs, deg, dst, MPI_UNWEIGHTED, MPI_INFO_NULL, 0, &dg);
        int in[1] = { left }, out[1] = { right };
        MPI_Dist_graph_create_adjacent(MPI_COMM_WORLD, 1, in, MPI_UNWEIGHTED, 1, out,
                                       MPI_UNWEIGHTED, MPI_INFO_NULL, 0, &dga);
    }
    /* even and odd ranks; leaders are world ranks 0 and 1 */
    half = split;
    MPI_Intercomm_create(half, 0, MPI_COMM_WORLD, me % 2 == 0 ? 1 : 0, 9, &inter);
    MPI_Intercomm_merge(inter, me % 2, &merged);
    {
        int ms;
        MPI_Comm_size(merged, &ms);
        expect("Intercomm_merge size", ms, n);
    }

    /* ── neighbourhood collectives on the periodic ring (2 in-neighbours) */
    {
        int c2[2] = { NS, NS }, d2[2] = { 0, NS };
        for (int i = 0; i < 2 * NS; i++) s[i] = me;
        MPI_Neighbor_allgather(s, NS, MPI_DOUBLE, r, NS, MPI_DOUBLE, ring);
        expect("Neighbor_allgather from left", r[0], left);
        MPI_Neighbor_allgatherv(s, NS, MPI_DOUBLE, r, c2, d2, MPI_DOUBLE, ring);
        MPI_Neighbor_alltoall(s, NS, MPI_DOUBLE, r, NS, MPI_DOUBLE, ring);
        MPI_Neighbor_alltoallv(s, c2, d2, MPI_DOUBLE, r, c2, d2, MPI_DOUBLE, ring);
        MPI_Ineighbor_allgather(s, NS, MPI_DOUBLE, r, NS, MPI_DOUBLE, ring, &q[0]);          MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ineighbor_allgatherv(s, NS, MPI_DOUBLE, r, c2, d2, MPI_DOUBLE, ring, &q[0]);     MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ineighbor_alltoall(s, NS, MPI_DOUBLE, r, NS, MPI_DOUBLE, ring, &q[0]);           MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Ineighbor_alltoallv(s, c2, d2, MPI_DOUBLE, r, c2, d2, MPI_DOUBLE, ring, &q[0]);  MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        expect("Ineighbor_alltoallv from right", r[NS], right);
    }

    /* ── one-sided ───────────────────────────────────────────────────── */
    {
        int *base = NULL, *sbase = NULL;
        MPI_Win wa, ws, wd;
        MPI_Win_allocate(sizeof(int), sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD, &base, &wa);
        MPI_Win_allocate_shared(sizeof(int), sizeof(int), MPI_INFO_NULL, shm, &sbase, &ws);
        MPI_Win_create_dynamic(MPI_INFO_NULL, MPI_COMM_WORLD, &wd);
        *base = 0;
        MPI_Barrier(MPI_COMM_WORLD);

        int one = 1, got = -1, cmp = 0, res = -1;
        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, right, 0, wa);
        MPI_Put(&one, 1, MPI_INT, right, 0, 1, MPI_INT, wa);
        MPI_Win_unlock(right, wa);
        MPI_Barrier(MPI_COMM_WORLD);
        expect("passive Put", *base, 1);
        MPI_Barrier(MPI_COMM_WORLD);     /* read before neighbours fetch-and-add */

        MPI_Win_lock_all(0, wa);
        MPI_Fetch_and_op(&one, &got, MPI_INT, right, 0, MPI_SUM, wa);
        MPI_Win_flush(right, wa);
        MPI_Compare_and_swap(&one, &cmp, &res, MPI_INT, right, 0, wa);
        MPI_Get_accumulate(&one, 1, MPI_INT, &res, 1, MPI_INT, right, 0, 1, MPI_INT, MPI_NO_OP, wa);
        MPI_Win_flush_all(wa);
        MPI_Rput(&one, 1, MPI_INT, right, 0, 1, MPI_INT, wa, &q[0]);
        MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Win_flush_local(right, wa);
        MPI_Rget(&got, 1, MPI_INT, right, 0, 1, MPI_INT, wa, &q[0]);
        MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Raccumulate(&one, 1, MPI_INT, right, 0, 1, MPI_INT, MPI_SUM, wa, &q[0]);
        MPI_Wait(&q[0], MPI_STATUS_IGNORE);
        MPI_Win_flush_local_all(wa);
        MPI_Win_sync(wa);
        MPI_Win_unlock_all(wa);
        MPI_Barrier(MPI_COMM_WORLD);

        /* PSCW with both neighbours (one group member when n == 2) */
        MPI_Group nb;
        int nbr[2] = { left, right };
        MPI_Group_incl(wgroup, left == right ? 1 : 2, nbr, &nb);
        MPI_Win_post(nb, 0, wa);
        MPI_Win_start(nb, 0, wa);
        MPI_Put(&one, 1, MPI_INT, right, 0, 1, MPI_INT, wa);
        MPI_Win_complete(wa);
        MPI_Win_wait(wa);
        MPI_Group_free(&nb);

        MPI_Win_free(&wa);
        MPI_Win_free(&ws);
        MPI_Win_free(&wd);
    }

    MPI_Comm_free(&merged);
    MPI_Comm_free(&inter);
    MPI_Comm_free(&dga);
    MPI_Comm_free(&dg);
    MPI_Comm_free(&sub);
    MPI_Comm_free(&grid);
    MPI_Comm_free(&ring);
    MPI_Comm_free(&cgroup);
    MPI_Comm_free(&created);
    MPI_Comm_free(&shm);
    MPI_Comm_free(&split);
    MPI_Comm_free(&dup);
    MPI_Group_free(&wgroup);

    if (me == 0) printf("mpi_coverage_validate: all self-checks passed on %d ranks\n", n);
    MPI_Finalize();
    return 0;
}
