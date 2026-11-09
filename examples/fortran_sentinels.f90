! fortran_sentinels.f90 — regression test for Fortran MPI sentinels
!
! miniprof's Fortran wrappers once converted arguments to C and called the
! C MPI API, which silently broke every Fortran sentinel: MPI_IN_PLACE was
! treated as a real (zero-filled) buffer, so in-place collectives returned
! wrong results in every miniprof mode. This program exercises each
! wrapped call that accepts a sentinel, checks the numerical result, and
! stops with a non-zero exit code on any mismatch. Run it under every
! MINIPROF_MODE (see `make run-fortran-sentinels`).
!
! It also makes a known number of calls per rank (one each), so trace and
! snapshot mode call counts can be checked for double counting.

program fortran_sentinels
  use mpi
  implicit none
  integer :: ierr, rank, np, i, root, nfail
  integer :: req(2), status(MPI_STATUS_SIZE), cnt
  integer, allocatable :: counts(:), displs(:)
  double precision :: a(4), b(4), want
  double precision, allocatable :: g(:), t(:)
  logical :: flag

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, np, ierr)
  nfail = 0
  root = 0
  allocate(g(np), t(np), counts(np), displs(np))
  counts = 1
  displs = [(i - 1, i = 1, np)]

  ! Allreduce, in place: sum of (rank+1) over ranks
  a = rank + 1
  call MPI_Allreduce(MPI_IN_PLACE, a, 4, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, ierr)
  want = np * (np + 1) / 2.0d0
  call expect('Allreduce IN_PLACE', all(a == want))

  ! Reduce, in place at root
  a = rank + 1
  if (rank == root) then
     call MPI_Reduce(MPI_IN_PLACE, a, 4, MPI_DOUBLE_PRECISION, MPI_SUM, root, MPI_COMM_WORLD, ierr)
     call expect('Reduce IN_PLACE at root', all(a == want))
  else
     call MPI_Reduce(a, b, 4, MPI_DOUBLE_PRECISION, MPI_SUM, root, MPI_COMM_WORLD, ierr)
  end if

  ! Allgather, in place: g(i) = i-1
  g = -1
  g(rank + 1) = rank
  call MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, g, 1, MPI_DOUBLE_PRECISION, &
                     MPI_COMM_WORLD, ierr)
  call expect('Allgather IN_PLACE', all(g == [(dble(i - 1), i = 1, np)]))

  ! Allgatherv, in place
  g = -1
  g(rank + 1) = rank
  call MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, g, counts, displs, &
                      MPI_DOUBLE_PRECISION, MPI_COMM_WORLD, ierr)
  call expect('Allgatherv IN_PLACE', all(g == [(dble(i - 1), i = 1, np)]))

  ! Gather, in place at root
  g = -1
  g(rank + 1) = rank
  if (rank == root) then
     call MPI_Gather(MPI_IN_PLACE, 1, MPI_DOUBLE_PRECISION, g, 1, MPI_DOUBLE_PRECISION, &
                     root, MPI_COMM_WORLD, ierr)
     call expect('Gather IN_PLACE at root', all(g == [(dble(i - 1), i = 1, np)]))
  else
     call MPI_Gather(g(rank + 1), 1, MPI_DOUBLE_PRECISION, t, 1, MPI_DOUBLE_PRECISION, &
                     root, MPI_COMM_WORLD, ierr)
  end if

  ! Scatter, in place at root: every rank ends with its own index
  g = [(dble(i - 1), i = 1, np)]
  if (rank == root) then
     call MPI_Scatter(g, 1, MPI_DOUBLE_PRECISION, MPI_IN_PLACE, 1, MPI_DOUBLE_PRECISION, &
                      root, MPI_COMM_WORLD, ierr)
     call expect('Scatter IN_PLACE at root', g(1) == 0d0)
  else
     a = -1
     call MPI_Scatter(g, 1, MPI_DOUBLE_PRECISION, a, 1, MPI_DOUBLE_PRECISION, &
                      root, MPI_COMM_WORLD, ierr)
     call expect('Scatter receive', a(1) == dble(rank))
  end if

  ! Alltoall, in place: element j on rank r becomes 100*j + r
  g = [(100d0 * rank + (i - 1), i = 1, np)]
  call MPI_Alltoall(MPI_IN_PLACE, 1, MPI_DOUBLE_PRECISION, g, 1, MPI_DOUBLE_PRECISION, &
                    MPI_COMM_WORLD, ierr)
  call expect('Alltoall IN_PLACE', all(g == [(100d0 * (i - 1) + rank, i = 1, np)]))

  ! Scan (inclusive prefix sum of rank+1)
  a = rank + 1
  call MPI_Scan(a, b, 4, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, ierr)
  call expect('Scan', all(b == (rank + 1) * (rank + 2) / 2.0d0))

  ! Sendrecv around a ring with MPI_STATUS_IGNORE
  a(1) = rank
  call MPI_Sendrecv(a(1), 1, MPI_DOUBLE_PRECISION, mod(rank + 1, np), 7, &
                    b(1), 1, MPI_DOUBLE_PRECISION, mod(rank - 1 + np, np), 7, &
                    MPI_COMM_WORLD, MPI_STATUS_IGNORE, ierr)
  call expect('Sendrecv STATUS_IGNORE', b(1) == dble(mod(rank - 1 + np, np)))

  ! Isend/Irecv + Waitall with MPI_STATUSES_IGNORE
  a(1) = 10 * rank
  call MPI_Irecv(b(1), 1, MPI_DOUBLE_PRECISION, mod(rank - 1 + np, np), 8, MPI_COMM_WORLD, req(1), ierr)
  call MPI_Isend(a(1), 1, MPI_DOUBLE_PRECISION, mod(rank + 1, np), 8, MPI_COMM_WORLD, req(2), ierr)
  call MPI_Waitall(2, req, MPI_STATUSES_IGNORE, ierr)
  call expect('Waitall STATUSES_IGNORE', b(1) == 10d0 * mod(rank - 1 + np, np))

  ! Probe with a real status, then receive the probed message
  a(1) = rank
  call MPI_Isend(a(1), 1, MPI_DOUBLE_PRECISION, mod(rank + 1, np), 9, MPI_COMM_WORLD, req(1), ierr)
  call MPI_Probe(mod(rank - 1 + np, np), 9, MPI_COMM_WORLD, status, ierr)
  call MPI_Get_count(status, MPI_DOUBLE_PRECISION, cnt, ierr)
  call expect('Probe status count', cnt == 1)
  call MPI_Recv(b(1), 1, MPI_DOUBLE_PRECISION, status(MPI_SOURCE), 9, MPI_COMM_WORLD, status, ierr)
  call MPI_Wait(req(1), MPI_STATUS_IGNORE, ierr)
  call expect('Recv after Probe', b(1) == dble(mod(rank - 1 + np, np)))
  call MPI_Iprobe(MPI_ANY_SOURCE, 99, MPI_COMM_WORLD, flag, MPI_STATUS_IGNORE, ierr)
  call expect('Iprobe (no message pending)', .not. flag)

  ! Bcast
  a = merge(42d0, 0d0, rank == root)
  call MPI_Bcast(a, 4, MPI_DOUBLE_PRECISION, root, MPI_COMM_WORLD, ierr)
  call expect('Bcast', all(a == 42d0))

  call MPI_Allreduce(MPI_IN_PLACE, nfail, 1, MPI_INTEGER, MPI_SUM, MPI_COMM_WORLD, ierr)
  if (rank == 0) then
     if (nfail == 0) then
        print '(A,I0,A)', 'fortran_sentinels: all checks passed on ', np, ' ranks'
     else
        print '(A,I0,A)', 'fortran_sentinels: ', nfail, ' check(s) FAILED'
     end if
  end if
  call MPI_Finalize(ierr)
  if (nfail /= 0) stop 1

contains

  subroutine expect(what, ok)
    character(*), intent(in) :: what
    logical, intent(in) :: ok
    if (.not. ok) then
       print '(A,I0,2A)', 'FAIL rank ', rank, ': ', what
       nfail = nfail + 1
    end if
  end subroutine expect

end program fortran_sentinels
