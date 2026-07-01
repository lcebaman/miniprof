/*
 * heat2d.c — 2D heat diffusion, MPI domain-decomposed
 *
 * Provides three distinct hot spots for miniprof to detect:
 *   compute_stencil()  — local stencil update (FP heavy)
 *   halo_exchange()    — MPI_Sendrecv between neighbours
 *   check_convergence()— MPI_Allreduce + norm compute
 *
 * Build & run:
 *   make
 *   mpirun -n 4 ./heat2d
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NX       2048    /* global grid width  */
#define NY       2048    /* global grid height */
#define NSTEPS   400     /* time steps         */
#define ALPHA    0.25    /* diffusivity        */

/* ── stencil ─────────────────────────────────────────────────────────────── */
static void compute_stencil(double *u, double *u_new,
                             int lnx, int lny)
{
    /* inner points only (halo rows excluded) */
    for (int i = 1; i < lnx-1; i++) {
        for (int j = 0; j < lny; j++) {
            u_new[i*lny + j] =
                u[i*lny + j] +
                ALPHA * (u[(i+1)*lny + j]
                       + u[(i-1)*lny + j]
                       + u[i*lny + (j+1 < lny ? j+1 : j)]
                       + u[i*lny + (j-1 >= 0  ? j-1 : j)]
                       - 4.0 * u[i*lny + j]);
        }
    }
}

/* ── halo exchange ───────────────────────────────────────────────────────── */
static void halo_exchange(double *u, int lnx, int lny,
                          int rank, int size)
{
    MPI_Status st;
    int up   = (rank > 0)        ? rank-1 : MPI_PROC_NULL;
    int down = (rank < size-1)   ? rank+1 : MPI_PROC_NULL;

    /* send bottom row down, receive top halo from above */
    MPI_Sendrecv(u + (lnx-2)*lny, lny, MPI_DOUBLE, down, 0,
                 u,                lny, MPI_DOUBLE, up,   0,
                 MPI_COMM_WORLD, &st);

    /* send top row up, receive bottom halo from below */
    MPI_Sendrecv(u + lny,         lny, MPI_DOUBLE, up,   1,
                 u + (lnx-1)*lny, lny, MPI_DOUBLE, down, 1,
                 MPI_COMM_WORLD, &st);
}

/* ── convergence ─────────────────────────────────────────────────────────── */
static double check_convergence(double *u, double *u_new,
                                int lnx, int lny)
{
    double local_norm = 0.0;
    for (int i = 1; i < lnx-1; i++)
        for (int j = 0; j < lny; j++) {
            double d = u_new[i*lny+j] - u[i*lny+j];
            local_norm += d*d;
        }

    double global_norm;
    MPI_Allreduce(&local_norm, &global_norm, 1, MPI_DOUBLE,
                  MPI_SUM, MPI_COMM_WORLD);
    return sqrt(global_norm);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    /* 1-D row decomposition */
    int lnx = NX / size + 2;   /* +2 halo rows */
    int lny = NY;

    double *u     = calloc(lnx * lny, sizeof(double));
    double *u_new = calloc(lnx * lny, sizeof(double));

    /* initial condition: Gaussian hot spot */
    int global_row_start = rank * (NX / size);
    for (int i = 1; i < lnx-1; i++) {
        int gi = global_row_start + (i-1);
        for (int j = 0; j < lny; j++) {
            double x = (gi - NX/2.0) / (NX/4.0);
            double y = (j  - NY/2.0) / (NY/4.0);
            u[i*lny + j] = exp(-(x*x + y*y));
        }
    }

    double norm = 0.0;
    for (int step = 0; step < NSTEPS; step++) {

        halo_exchange(u, lnx, lny, rank, size);
        compute_stencil(u, u_new, lnx, lny);
        norm = check_convergence(u, u_new, lnx, lny);

        /* swap pointers */
        double *tmp = u; u = u_new; u_new = tmp;
    }

    if (rank == 0)
        printf("heat2d: %d steps complete, final norm = %e\n", NSTEPS, norm);

    free(u);
    free(u_new);
    MPI_Finalize();
    return 0;
}
