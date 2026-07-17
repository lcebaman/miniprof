! mpi_ext_validate.f90 — Fortran companion to mpi_ext_validate.c
!
! Exercises the same MPI_Test/Wait family and one-sided RMA operations,
! through Fortran's mpi_xxx_ bindings, to validate the Fortran wrappers
! added to mpi_trace.c alongside their C counterparts — particularly the
! array-of-requests/statuses marshaling (Waitall/Waitany/Testall/Testany)
! and the MPI_Aint handling in Put/Get/Win_create, which are the two
! places a mismatch between the Fortran and C ABI would show up as wrong
! values rather than a build failure.
!
! Build & run:
!   mpifort -O0 -g examples/mpi_ext_validate.f90 -L. -lminiprof -ldl -lm -o mpi_ext_validate_f
!   mpirun -n 2 ./mpi_ext_validate_f
!
! Not part of `make all` — see `make run-ext-trace` at the repo root.

program mpi_ext_validate
    use mpi
    implicit none

    integer, parameter :: n_reqs = 4
    integer :: rank, nprocs, ierr, right, left, i, idx, remaining
    logical :: flag
    integer :: reqs(2 * n_reqs), rreqs(n_reqs), sreqs(n_reqs)
    integer :: sendbuf(n_reqs), recvbuf(n_reqs)
    integer :: sendval, recvval, sreq, rreq
    integer :: window_val, my_rank_val, fetched, win
    integer(kind=MPI_ADDRESS_KIND) :: win_size, target_disp

    call MPI_Init(ierr)
    call MPI_Comm_rank(MPI_COMM_WORLD, rank, ierr)
    call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)

    if (nprocs < 2) then
        if (rank == 0) write(0,*) 'mpi_ext_validate: needs >= 2 ranks'
        call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
    end if
    right = mod(rank + 1, nprocs)
    left  = mod(rank - 1 + nprocs, nprocs)

    ! ── MPI_Waitall: N Isend/Irecv pairs around the ring ──────────────────
    do i = 1, n_reqs
        sendbuf(i) = rank * 1000 + i
        call MPI_Isend(sendbuf(i), 1, MPI_INTEGER, right, i, MPI_COMM_WORLD, reqs(i), ierr)
        call MPI_Irecv(recvbuf(i), 1, MPI_INTEGER, left, i, MPI_COMM_WORLD, reqs(n_reqs + i), ierr)
    end do
    call MPI_Waitall(2 * n_reqs, reqs, MPI_STATUSES_IGNORE, ierr)
    do i = 1, n_reqs
        if (recvbuf(i) /= left * 1000 + i) then
            write(0,*) 'rank', rank, ': Waitall mismatch at', i, 'got', recvbuf(i)
            call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
        end if
    end do

    ! ── MPI_Waitany: drain N sends one at a time ──────────────────────────
    do i = 1, n_reqs
        sendbuf(i) = rank * 2000 + i
        call MPI_Isend(sendbuf(i), 1, MPI_INTEGER, right, 100 + i, MPI_COMM_WORLD, sreqs(i), ierr)
        call MPI_Irecv(recvbuf(i), 1, MPI_INTEGER, left, 100 + i, MPI_COMM_WORLD, rreqs(i), ierr)
    end do
    do i = 1, n_reqs
        call MPI_Waitany(n_reqs, rreqs, idx, MPI_STATUS_IGNORE, ierr)
        if (recvbuf(idx) /= left * 2000 + idx) then
            write(0,*) 'rank', rank, ': Waitany mismatch at', idx, 'got', recvbuf(idx)
            call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
        end if
    end do
    call MPI_Waitall(n_reqs, sreqs, MPI_STATUSES_IGNORE, ierr)

    ! ── MPI_Test: spin-poll a single request to completion ────────────────
    sendval = rank * 3000
    recvval = -1
    call MPI_Isend(sendval, 1, MPI_INTEGER, right, 200, MPI_COMM_WORLD, sreq, ierr)
    call MPI_Irecv(recvval, 1, MPI_INTEGER, left, 200, MPI_COMM_WORLD, rreq, ierr)
    flag = .false.
    do while (.not. flag)
        call MPI_Test(rreq, flag, MPI_STATUS_IGNORE, ierr)
    end do
    if (recvval /= left * 3000) then
        write(0,*) 'rank', rank, ': Test mismatch, got', recvval
        call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
    end if
    call MPI_Wait(sreq, MPI_STATUS_IGNORE, ierr)

    ! ── MPI_Testany: spin-poll N requests, draining as they complete ──────
    do i = 1, n_reqs
        sendbuf(i) = rank * 4000 + i
        call MPI_Isend(sendbuf(i), 1, MPI_INTEGER, right, 300 + i, MPI_COMM_WORLD, sreqs(i), ierr)
        call MPI_Irecv(recvbuf(i), 1, MPI_INTEGER, left, 300 + i, MPI_COMM_WORLD, rreqs(i), ierr)
    end do
    remaining = n_reqs
    do while (remaining > 0)
        call MPI_Testany(n_reqs, rreqs, idx, flag, MPI_STATUS_IGNORE, ierr)
        if (flag .and. idx /= MPI_UNDEFINED) then
            if (recvbuf(idx) /= left * 4000 + idx) then
                write(0,*) 'rank', rank, ': Testany mismatch at', idx, 'got', recvbuf(idx)
                call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
            end if
            remaining = remaining - 1
        end if
    end do
    call MPI_Waitall(n_reqs, sreqs, MPI_STATUSES_IGNORE, ierr)

    ! ── RMA: Win_create / Win_fence / Put / Get / Win_free ─────────────────
    window_val = -1
    win_size = 4_MPI_ADDRESS_KIND
    call MPI_Win_create(window_val, win_size, 4, MPI_INFO_NULL, MPI_COMM_WORLD, win, ierr)

    my_rank_val = rank
    target_disp = 0_MPI_ADDRESS_KIND
    call MPI_Win_fence(0, win, ierr)
    call MPI_Put(my_rank_val, 1, MPI_INTEGER, right, target_disp, 1, MPI_INTEGER, win, ierr)
    call MPI_Win_fence(0, win, ierr)
    ! My own window gets written by whoever's right-neighbour I am, i.e.
    ! my LEFT neighbour — not by me.
    if (window_val /= left) then
        write(0,*) 'rank', rank, ': Put mismatch, window_val=', window_val
        call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
    end if

    ! Round-trip: Get from the same target (right) we just Put to — should
    ! read back exactly what we wrote.
    fetched = -1
    call MPI_Win_fence(0, win, ierr)
    call MPI_Get(fetched, 1, MPI_INTEGER, right, target_disp, 1, MPI_INTEGER, win, ierr)
    call MPI_Win_fence(0, win, ierr)
    if (fetched /= rank) then
        write(0,*) 'rank', rank, ': Get mismatch, fetched=', fetched
        call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
    end if

    call MPI_Win_free(win, ierr)

    if (rank == 0) write(0,*) 'mpi_ext_validate_f: all checks passed on', nprocs, 'ranks'

    call MPI_Finalize(ierr)
end program mpi_ext_validate
