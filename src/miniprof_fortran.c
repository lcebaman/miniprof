/*
 * miniprof_fortran.c — Fortran PMPI binding wrappers (init/finalize)
 *
 * The C wrappers in miniprof.c (MPI_Init, MPI_Init_thread, MPI_Finalize)
 * only intercept code that calls the C MPI API directly. Fortran's
 * `mpi`/`mpi_f08` modules do NOT call those symbols — they call
 * separately-named, separately-mangled bindings (mpi_init_,
 * mpi_finalize_, etc.) that live in the Fortran binding shim
 * (libmpi_mpifh.so for Open MPI) and bypass the C wrappers entirely.
 * Confirmed by disassembling a real gfortran+Open MPI binary: the call
 * site targets mpi_init_@plt, never MPI_Init — so without these
 * wrappers, miniprof silently never starts for any Fortran MPI program.
 *
 * Fortran's calling convention passes every argument by reference
 * (pointer) and uses a trailing integer output parameter for the error
 * code instead of a C-style return value, so these wrappers have a
 * different shape from their C counterparts even though they do the
 * same job.
 *
 * WHY THIS IS A SEPARATE TRANSLATION UNIT (not part of miniprof.c):
 * a static archive only pulls in a .o file when something references a
 * symbol defined in it. mpi_init_() here references pmpi_init_(), which
 * only exists in libmpi_mpifh.so — a library mpicc does NOT link by
 * default (only mpifort does). If these wrappers lived in the same .o as
 * the always-needed C wrappers, linking ANY program against
 * libminiprof.a — including plain C programs — would drag in this
 * dependency and fail with "undefined reference to pmpi_init_". Keeping
 * them separate means the linker only pulls this object (and its
 * pmpi_*_ dependency) in for programs that actually call into a Fortran
 * MPI binding in the first place, i.e. ones linked via mpifort, which
 * does provide libmpi_mpifh.
 *
 * Defined with trailing-underscore mangling (gfortran's default, and
 * the convention most Linux Fortran compilers use). If you're building
 * with a compiler that mangles differently (e.g. some Intel ifort
 * configurations use no trailing underscore), add the corresponding
 * alias following the same pattern — call the existing miniprof_init()/
 * miniprof_finalize() C API, which is mode/compiler-agnostic.
 */

#include "../include/miniprof.h"
#include <mpi.h>

extern void pmpi_init_(MPI_Fint *ierr);
extern void pmpi_init_thread_(MPI_Fint *required, MPI_Fint *provided,
                              MPI_Fint *ierr);
extern void pmpi_finalize_(MPI_Fint *ierr);

void mpi_init_(MPI_Fint *ierr)
{
    pmpi_init_(ierr);
    if (ierr && *ierr == MPI_SUCCESS)
        miniprof_init(MINIPROF_DEFAULT_INTERVAL_MS);
}

void mpi_init_thread_(MPI_Fint *required, MPI_Fint *provided, MPI_Fint *ierr)
{
    pmpi_init_thread_(required, provided, ierr);
    if (ierr && *ierr == MPI_SUCCESS)
        miniprof_init(MINIPROF_DEFAULT_INTERVAL_MS);
}

void mpi_finalize_(MPI_Fint *ierr)
{
    miniprof_finalize();
    pmpi_finalize_(ierr);
}
