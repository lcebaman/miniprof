/*
 * mpi_ext_validate.c — sanity check for the MPI_Test/Wait family and
 * one-sided RMA wrappers added to mpi_trace.c.
 *
 * Exercises, in order, every wrapper that didn't exist before this slice:
 *   MPI_Waitall   — post N Isend/Irecv pairs around a ring, wait on all
 *   MPI_Waitany   — post N sends, drain them one at a time via Waitany
 *   MPI_Test      — spin-poll a single request to completion
 *   MPI_Testany   — spin-poll N requests, draining via Testany
 *   MPI_Win_create/MPI_Win_fence/MPI_Put/MPI_Get/MPI_Win_free
 *                 — each rank puts its rank number into its right
 *                   neighbour's window, fences, then gets it back from
 *                   its left neighbour's window and checks the value
 *
 * Not part of `make all` — see `make run-ext-trace` at the repo root,
 * which runs this under MINIPROF_MODE=trace and checks every new
 * wrapper shows up in Table 2 (MPI Call Tracing) with the right call
 * count. Needs >= 2 ranks.
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define N_REQS 4

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size < 2) {
        if (rank == 0) fprintf(stderr, "mpi_ext_validate: needs >= 2 ranks\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int right = (rank + 1) % size;
    int left  = (rank - 1 + size) % size;

    /* ── MPI_Waitall: N Isend/Irecv pairs around the ring ────────────────── */
    {
        MPI_Request reqs[2 * N_REQS];
        int sendbuf[N_REQS], recvbuf[N_REQS];
        for (int i = 0; i < N_REQS; i++) sendbuf[i] = rank * 1000 + i;
        for (int i = 0; i < N_REQS; i++) {
            MPI_Isend(&sendbuf[i], 1, MPI_INT, right, i, MPI_COMM_WORLD, &reqs[i]);
            MPI_Irecv(&recvbuf[i], 1, MPI_INT, left, i, MPI_COMM_WORLD, &reqs[N_REQS + i]);
        }
        MPI_Waitall(2 * N_REQS, reqs, MPI_STATUSES_IGNORE);
        for (int i = 0; i < N_REQS; i++) {
            int expect = left * 1000 + i;
            if (recvbuf[i] != expect) {
                fprintf(stderr, "rank %d: Waitall mismatch at %d: got %d, expected %d\n",
                        rank, i, recvbuf[i], expect);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }
    }

    /* ── MPI_Waitany: drain N sends one at a time ────────────────────────── */
    {
        MPI_Request reqs[N_REQS];
        int sendbuf[N_REQS], recvbuf[N_REQS];
        for (int i = 0; i < N_REQS; i++) sendbuf[i] = rank * 2000 + i;
        for (int i = 0; i < N_REQS; i++)
            MPI_Isend(&sendbuf[i], 1, MPI_INT, right, 100 + i, MPI_COMM_WORLD, &reqs[i]);

        MPI_Request rreqs[N_REQS];
        for (int i = 0; i < N_REQS; i++)
            MPI_Irecv(&recvbuf[i], 1, MPI_INT, left, 100 + i, MPI_COMM_WORLD, &rreqs[i]);

        for (int done = 0; done < N_REQS; done++) {
            int idx;
            MPI_Waitany(N_REQS, rreqs, &idx, MPI_STATUS_IGNORE);
            int expect = left * 2000 + idx;
            if (recvbuf[idx] != expect) {
                fprintf(stderr, "rank %d: Waitany mismatch at %d: got %d, expected %d\n",
                        rank, idx, recvbuf[idx], expect);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }
        MPI_Waitall(N_REQS, reqs, MPI_STATUSES_IGNORE); /* drain the sends */
    }

    /* ── MPI_Test: spin-poll a single request to completion ─────────────── */
    {
        int sendbuf = rank * 3000, recvbuf = -1;
        MPI_Request sreq, rreq;
        MPI_Isend(&sendbuf, 1, MPI_INT, right, 200, MPI_COMM_WORLD, &sreq);
        MPI_Irecv(&recvbuf, 1, MPI_INT, left, 200, MPI_COMM_WORLD, &rreq);

        int flag = 0;
        while (!flag) MPI_Test(&rreq, &flag, MPI_STATUS_IGNORE);
        if (recvbuf != left * 3000) {
            fprintf(stderr, "rank %d: Test mismatch: got %d, expected %d\n",
                    rank, recvbuf, left * 3000);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        MPI_Wait(&sreq, MPI_STATUS_IGNORE);
    }

    /* ── MPI_Testany: spin-poll N requests, draining as they complete ───── */
    {
        MPI_Request reqs[N_REQS];
        int sendbuf[N_REQS], recvbuf[N_REQS];
        for (int i = 0; i < N_REQS; i++) sendbuf[i] = rank * 4000 + i;
        for (int i = 0; i < N_REQS; i++)
            MPI_Isend(&sendbuf[i], 1, MPI_INT, right, 300 + i, MPI_COMM_WORLD, &reqs[i]);

        MPI_Request rreqs[N_REQS];
        for (int i = 0; i < N_REQS; i++)
            MPI_Irecv(&recvbuf[i], 1, MPI_INT, left, 300 + i, MPI_COMM_WORLD, &rreqs[i]);

        int remaining = N_REQS;
        while (remaining > 0) {
            int idx, flag;
            MPI_Testany(N_REQS, rreqs, &idx, &flag, MPI_STATUS_IGNORE);
            if (flag && idx != MPI_UNDEFINED) {
                int expect = left * 4000 + idx;
                if (recvbuf[idx] != expect) {
                    fprintf(stderr, "rank %d: Testany mismatch at %d: got %d, expected %d\n",
                            rank, idx, recvbuf[idx], expect);
                    MPI_Abort(MPI_COMM_WORLD, 1);
                }
                remaining--;
            }
        }
        MPI_Waitall(N_REQS, reqs, MPI_STATUSES_IGNORE);
    }

    /* ── RMA: Win_create / Win_fence / Put / Get / Win_free ──────────────── */
    {
        int window_val = -1;
        MPI_Win win;
        MPI_Win_create(&window_val, sizeof(int), sizeof(int), MPI_INFO_NULL,
                       MPI_COMM_WORLD, &win);

        int my_rank_val = rank;
        MPI_Win_fence(0, win);
        MPI_Put(&my_rank_val, 1, MPI_INT, right, 0, 1, MPI_INT, win);
        MPI_Win_fence(0, win);
        /* My own window gets written by whoever's right-neighbour I am,
           i.e. my LEFT neighbour (they call MPI_Put targeting `right`,
           which for them equals my rank) — not by me. */
        if (window_val != left) {
            fprintf(stderr, "rank %d: Put mismatch: window_val=%d, expected %d\n",
                    rank, window_val, left);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        /* Round-trip: Get from the same target (right) we just Put to —
           should read back exactly what we wrote. */
        int fetched = -1;
        MPI_Win_fence(0, win);
        MPI_Get(&fetched, 1, MPI_INT, right, 0, 1, MPI_INT, win);
        MPI_Win_fence(0, win);
        if (fetched != rank) {
            fprintf(stderr, "rank %d: Get mismatch: fetched=%d, expected %d\n",
                    rank, fetched, rank);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        MPI_Win_free(&win);
    }

    if (rank == 0)
        fprintf(stderr, "mpi_ext_validate: all checks passed on %d ranks\n", size);

    MPI_Finalize();
    return 0;
}
