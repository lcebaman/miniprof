! heat1d.f90 — 1D heat diffusion, MPI domain-decomposed
!
! Fortran companion to examples/heat2d.c, used to verify miniprof works
! transparently with Fortran MPI codes via the Fortran PMPI binding
! wrappers in src/miniprof_fortran.c and src/mpi_trace.c. No source
! changes are needed beyond linking against libminiprof.a — MPI_Init/
! MPI_Finalize interception happens automatically, exactly as it does
! for C codes.
!
! Build & run:
!   mpifort -O0 -g examples/heat1d.f90 -L. -lminiprof -ldl -lm -o heat1d
!   mpirun -n 4 ./heat1d
!
! Note: -O0 is used here (rather than -O2 as in the C example) because
! gfortran inlines small internal `contains` subroutines very
! aggressively — even more readily than C's `static` functions — which
! would remove compute_step/exchange_halo/check_norm as separate
! symbols entirely. See README.md's "Fortran support" section.

program heat1d
    use mpi
    implicit none

    integer, parameter :: nx = 1000000
    integer, parameter :: nsteps = 100
    double precision, allocatable :: u(:), u_new(:)
    integer :: rank, nprocs, ierr, step, i
    double precision :: norm

    call MPI_Init(ierr)
    call MPI_Comm_rank(MPI_COMM_WORLD, rank, ierr)
    call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)

    allocate(u(nx), u_new(nx))
    do i = 1, nx
        u(i) = sin(dble(i) / 1000.0d0)
    end do

    do step = 1, nsteps
        call compute_step(u, u_new, nx)
        call exchange_halo(u_new, nx, rank, nprocs)
        call check_norm(u_new, nx, norm)
        u = u_new
    end do

    if (rank == 0) print *, 'Final norm:', norm

    deallocate(u, u_new)
    call MPI_Finalize(ierr)

contains

    subroutine compute_step(u, u_new, n)
        integer, intent(in) :: n
        double precision, intent(in)  :: u(n)
        double precision, intent(out) :: u_new(n)
        integer :: j
        do j = 2, n-1
            u_new(j) = u(j) + 0.25d0 * (u(j+1) + u(j-1) - 2.0d0*u(j))
        end do
        u_new(1) = u(1)
        u_new(n) = u(n)
    end subroutine compute_step

    subroutine exchange_halo(u, n, rank, nprocs)
        integer, intent(in) :: n, rank, nprocs
        double precision, intent(inout) :: u(n)
        integer :: ierr, status(MPI_STATUS_SIZE)
        integer :: up, down
        double precision :: send_down, send_up, recv_from_up, recv_from_down

        ! Two Sendrecv calls, mirroring examples/heat2d.c's halo_exchange:
        ! every rank both sends AND receives in each call, regardless of
        ! its position in the chain — MPI_PROC_NULL makes the call a safe
        ! no-op at the two domain boundaries. A single one-directional
        ! Sendrecv (each rank only talking to rank+1) deadlocks: the last
        ! rank in the chain would never receive a matching send from
        ! anyone, so an earlier rank blocks forever waiting for it.
        up   = rank - 1
        if (up < 0) up = MPI_PROC_NULL
        down = rank + 1
        if (down >= nprocs) down = MPI_PROC_NULL

        ! send last element down, receive into a left halo from above
        send_down = u(n)
        call MPI_Sendrecv(send_down, 1, MPI_DOUBLE_PRECISION, down, 0, &
                          recv_from_up, 1, MPI_DOUBLE_PRECISION, up, 0, &
                          MPI_COMM_WORLD, status, ierr)

        ! send first element up, receive into a right halo from below
        send_up = u(1)
        call MPI_Sendrecv(send_up, 1, MPI_DOUBLE_PRECISION, up, 1, &
                          recv_from_down, 1, MPI_DOUBLE_PRECISION, down, 1, &
                          MPI_COMM_WORLD, status, ierr)

        ! fold the received halo values back into u's boundary elements
        ! (kept simple/approximate for profiler-test purposes — this is
        ! not meant to be a physically rigorous stencil update)
        if (up /= MPI_PROC_NULL)   u(1) = 0.5d0 * (u(1) + recv_from_up)
        if (down /= MPI_PROC_NULL) u(n) = 0.5d0 * (u(n) + recv_from_down)
    end subroutine exchange_halo

    subroutine check_norm(u, n, norm)
        integer, intent(in) :: n
        double precision, intent(in) :: u(n)
        double precision, intent(out) :: norm
        double precision :: local_norm
        integer :: ierr, j
        local_norm = 0.0d0
        do j = 1, n
            local_norm = local_norm + u(j)*u(j)
        end do
        call MPI_Allreduce(local_norm, norm, 1, MPI_DOUBLE_PRECISION, &
                           MPI_SUM, MPI_COMM_WORLD, ierr)
        norm = sqrt(norm)
    end subroutine check_norm

end program heat1d
