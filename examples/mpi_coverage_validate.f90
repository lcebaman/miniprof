! mpi_coverage_validate.f90 — the calls of mpi_coverage_validate.c through
! the Fortran `mpi` module bindings, with the same expected call counts and
! byte totals (tools/validate_mpi_coverage.py checks both). LOGICAL
! arguments, MPI_UNWEIGHTED, MPI_Message handles and the
! INTEGER(MPI_ADDRESS_KIND) base pointer of MPI_Win_allocate all pass
! through miniprof's Fortran wrappers here. Needs >= 2 ranks.
program mpi_coverage_validate
  use mpi
  use iso_c_binding
  implicit none
  integer, parameter :: NS = 10, NB = 100, K_ALL = 5, K_ONE = 3
  integer :: ierr, me, n, right, left, i, k, flag, ms
  integer :: pr(2), q(2), msg
  double precision :: s(NB*4), r(NB*4)
  double precision, allocatable :: big_s(:), big_r(:)
  integer, allocatable :: cnt(:), dsp(:)
  integer :: v(4), w(4), c2(2), d2(2)
  integer :: wgroup, nb_group, nbr(2)
  integer :: dup, split, shm, created, cgroup, ring, grid, sub, dg, dga, inter, merged
  integer :: dims1(1), dims2(2), srcs(1), deg(1), dst(1), inn(1), outn(1)
  logical :: per1(1), per2(2), keep(2)
  integer :: wa, ws, wd, one, got, cmp, res
  integer(kind=MPI_ADDRESS_KIND) :: wsize, baseptr, sbaseptr, disp0
  type(c_ptr) :: cp
  integer, pointer :: bv

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, me, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, n, ierr)
  if (n < 2) then
    if (me == 0) print *, 'mpi_coverage_validate_f: needs >= 2 ranks'
    call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  end if
  right = mod(me + 1, n)
  left = mod(me - 1 + n, n)

  ! ── persistent ──────────────────────────────────────────────────────
  call MPI_Send_init(s, NS, MPI_DOUBLE_PRECISION, right, 1, MPI_COMM_WORLD, pr(1), ierr)
  call MPI_Recv_init(r, NS, MPI_DOUBLE_PRECISION, left, 1, MPI_COMM_WORLD, pr(2), ierr)
  do k = 0, K_ALL - 1
    s(1:NS) = me * 100 + k
    call MPI_Startall(2, pr, ierr)
    call MPI_Waitall(2, pr, MPI_STATUSES_IGNORE, ierr)
    call expect('persistent Startall data', r(1), dble(left * 100 + k))
  end do
  do k = 1, K_ONE
    call MPI_Start(pr(2), ierr)
    call MPI_Start(pr(1), ierr)
    call MPI_Waitall(2, pr, MPI_STATUSES_IGNORE, ierr)
  end do
  call MPI_Request_free(pr(1), ierr)
  call MPI_Request_free(pr(2), ierr)

  ! ── matched probe ───────────────────────────────────────────────────
  v = me
  call MPI_Isend(v, 4, MPI_INTEGER, right, 2, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Mprobe(left, 2, MPI_COMM_WORLD, msg, MPI_STATUS_IGNORE, ierr)
  call MPI_Mrecv(w, 4, MPI_INTEGER, msg, MPI_STATUS_IGNORE, ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call expect('Mrecv data', dble(w(4)), dble(left))
  call MPI_Isend(v, 4, MPI_INTEGER, right, 3, MPI_COMM_WORLD, q(1), ierr)
  flag = 0
  do while (flag == 0)
    call improbe_once(flag)
  end do
  call MPI_Imrecv(w, 4, MPI_INTEGER, msg, q(2), ierr)
  call MPI_Waitall(2, q, MPI_STATUSES_IGNORE, ierr)
  call expect('Imrecv data', dble(w(1)), dble(left))

  ! ── nonblocking collectives ─────────────────────────────────────────
  allocate(big_s(n*NS), big_r(n*NS), cnt(n), dsp(n))
  do i = 1, n
    cnt(i) = NS
    dsp(i) = (i - 1) * NS
  end do
  big_s = 0d0
  s(1:NB) = 1d0
  call MPI_Ibarrier(MPI_COMM_WORLD, q(1), ierr);  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ibcast(s, NB, MPI_DOUBLE_PRECISION, 0, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ireduce(s, r, NB, MPI_DOUBLE_PRECISION, MPI_SUM, 0, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  if (me == 0) call expect('Ireduce sum', r(1), dble(n))
  call MPI_Iallreduce(s, r, NB, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call expect('Iallreduce sum', r(NB), dble(n))
  call MPI_Igather(s, NS, MPI_DOUBLE_PRECISION, big_r, NS, MPI_DOUBLE_PRECISION, 0, &
                   MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Igatherv(s, NS, MPI_DOUBLE_PRECISION, big_r, cnt, dsp, MPI_DOUBLE_PRECISION, 0, &
                    MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Iallgather(s, NS, MPI_DOUBLE_PRECISION, big_r, NS, MPI_DOUBLE_PRECISION, &
                      MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Iallgatherv(s, NS, MPI_DOUBLE_PRECISION, big_r, cnt, dsp, MPI_DOUBLE_PRECISION, &
                       MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Iscatter(big_s, NS, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, 0, &
                    MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Iscatterv(big_s, cnt, dsp, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, 0, &
                     MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ialltoall(big_s, NS, MPI_DOUBLE_PRECISION, big_r, NS, MPI_DOUBLE_PRECISION, &
                     MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ialltoallv(big_s, cnt, dsp, MPI_DOUBLE_PRECISION, big_r, cnt, dsp, &
                      MPI_DOUBLE_PRECISION, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  big_s = 1d0
  call MPI_Ireduce_scatter(big_s, r, cnt, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call expect('Ireduce_scatter sum', r(1), dble(n))
  call MPI_Ireduce_scatter_block(big_s, r, NS, MPI_DOUBLE_PRECISION, MPI_SUM, &
                                 MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Iscan(s, r, NB, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call expect('Iscan prefix', r(1), dble(me + 1))
  call MPI_Iexscan(s, r, NB, MPI_DOUBLE_PRECISION, MPI_SUM, MPI_COMM_WORLD, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  if (me > 0) call expect('Iexscan prefix', r(1), dble(me))

  ! ── communicators and topologies ────────────────────────────────────
  call MPI_Comm_group(MPI_COMM_WORLD, wgroup, ierr)
  call MPI_Comm_dup(MPI_COMM_WORLD, dup, ierr)
  call MPI_Comm_split(MPI_COMM_WORLD, mod(me, 2), me, split, ierr)
  call MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, me, MPI_INFO_NULL, shm, ierr)
  call MPI_Comm_create(MPI_COMM_WORLD, wgroup, created, ierr)
  call MPI_Comm_create_group(MPI_COMM_WORLD, wgroup, 7, cgroup, ierr)
  dims1(1) = n
  per1(1) = .true.
  call MPI_Cart_create(MPI_COMM_WORLD, 1, dims1, per1, .false., ring, ierr)
  dims2 = (/ n, 1 /)
  per2 = .false.
  keep = (/ .true., .false. /)
  call MPI_Cart_create(MPI_COMM_WORLD, 2, dims2, per2, .false., grid, ierr)
  call MPI_Cart_sub(grid, keep, sub, ierr)
  srcs(1) = me
  deg(1) = 1
  dst(1) = right
  call MPI_Dist_graph_create(MPI_COMM_WORLD, 1, srcs, deg, dst, MPI_UNWEIGHTED, &
                             MPI_INFO_NULL, .false., dg, ierr)
  inn(1) = left
  outn(1) = right
  call MPI_Dist_graph_create_adjacent(MPI_COMM_WORLD, 1, inn, MPI_UNWEIGHTED, 1, outn, &
                                      MPI_UNWEIGHTED, MPI_INFO_NULL, .false., dga, ierr)
  if (mod(me, 2) == 0) then
    call MPI_Intercomm_create(split, 0, MPI_COMM_WORLD, 1, 9, inter, ierr)
  else
    call MPI_Intercomm_create(split, 0, MPI_COMM_WORLD, 0, 9, inter, ierr)
  end if
  call MPI_Intercomm_merge(inter, mod(me, 2) == 1, merged, ierr)
  call MPI_Comm_size(merged, ms, ierr)
  call expect('Intercomm_merge size', dble(ms), dble(n))

  ! ── neighbourhood collectives on the periodic ring ─────────────────
  c2 = (/ NS, NS /)
  d2 = (/ 0, NS /)
  s(1:2*NS) = me
  call MPI_Neighbor_allgather(s, NS, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, ring, ierr)
  call expect('Neighbor_allgather from left', r(1), dble(left))
  call MPI_Neighbor_allgatherv(s, NS, MPI_DOUBLE_PRECISION, r, c2, d2, MPI_DOUBLE_PRECISION, ring, ierr)
  call MPI_Neighbor_alltoall(s, NS, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, ring, ierr)
  call MPI_Neighbor_alltoallv(s, c2, d2, MPI_DOUBLE_PRECISION, r, c2, d2, MPI_DOUBLE_PRECISION, &
                              ring, ierr)
  call MPI_Ineighbor_allgather(s, NS, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, ring, &
                               q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ineighbor_allgatherv(s, NS, MPI_DOUBLE_PRECISION, r, c2, d2, MPI_DOUBLE_PRECISION, &
                                ring, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ineighbor_alltoall(s, NS, MPI_DOUBLE_PRECISION, r, NS, MPI_DOUBLE_PRECISION, ring, &
                              q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Ineighbor_alltoallv(s, c2, d2, MPI_DOUBLE_PRECISION, r, c2, d2, MPI_DOUBLE_PRECISION, &
                               ring, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call expect('Ineighbor_alltoallv from right', r(NS + 1), dble(right))

  ! ── one-sided ───────────────────────────────────────────────────────
  wsize = 4
  disp0 = 0
  call MPI_Win_allocate(wsize, 4, MPI_INFO_NULL, MPI_COMM_WORLD, baseptr, wa, ierr)
  call MPI_Win_allocate_shared(wsize, 4, MPI_INFO_NULL, shm, sbaseptr, ws, ierr)
  call MPI_Win_create_dynamic(MPI_INFO_NULL, MPI_COMM_WORLD, wd, ierr)
  cp = transfer(baseptr, cp)
  call c_f_pointer(cp, bv)
  bv = 0
  call MPI_Barrier(MPI_COMM_WORLD, ierr)
  one = 1
  got = -1
  cmp = 0
  res = -1
  call MPI_Win_lock(MPI_LOCK_EXCLUSIVE, right, 0, wa, ierr)
  call MPI_Put(one, 1, MPI_INTEGER, right, disp0, 1, MPI_INTEGER, wa, ierr)
  call MPI_Win_unlock(right, wa, ierr)
  call MPI_Barrier(MPI_COMM_WORLD, ierr)
  call expect('passive Put', dble(bv), 1d0)
  call MPI_Barrier(MPI_COMM_WORLD, ierr)   ! read before neighbours fetch-and-add
  call MPI_Win_lock_all(0, wa, ierr)
  call MPI_Fetch_and_op(one, got, MPI_INTEGER, right, disp0, MPI_SUM, wa, ierr)
  call MPI_Win_flush(right, wa, ierr)
  call MPI_Compare_and_swap(one, cmp, res, MPI_INTEGER, right, disp0, wa, ierr)
  call MPI_Get_accumulate(one, 1, MPI_INTEGER, res, 1, MPI_INTEGER, right, disp0, 1, &
                          MPI_INTEGER, MPI_NO_OP, wa, ierr)
  call MPI_Win_flush_all(wa, ierr)
  call MPI_Rput(one, 1, MPI_INTEGER, right, disp0, 1, MPI_INTEGER, wa, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Win_flush_local(right, wa, ierr)
  call MPI_Rget(got, 1, MPI_INTEGER, right, disp0, 1, MPI_INTEGER, wa, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Raccumulate(one, 1, MPI_INTEGER, right, disp0, 1, MPI_INTEGER, MPI_SUM, wa, q(1), ierr)
  call MPI_Wait(q(1), MPI_STATUS_IGNORE, ierr)
  call MPI_Win_flush_local_all(wa, ierr)
  call MPI_Win_sync(wa, ierr)
  call MPI_Win_unlock_all(wa, ierr)
  call MPI_Barrier(MPI_COMM_WORLD, ierr)
  nbr = (/ left, right /)
  if (left == right) then
    call MPI_Group_incl(wgroup, 1, nbr, nb_group, ierr)
  else
    call MPI_Group_incl(wgroup, 2, nbr, nb_group, ierr)
  end if
  call MPI_Win_post(nb_group, 0, wa, ierr)
  call MPI_Win_start(nb_group, 0, wa, ierr)
  call MPI_Put(one, 1, MPI_INTEGER, right, disp0, 1, MPI_INTEGER, wa, ierr)
  call MPI_Win_complete(wa, ierr)
  call MPI_Win_wait(wa, ierr)
  call MPI_Group_free(nb_group, ierr)
  call MPI_Win_free(wa, ierr)
  call MPI_Win_free(ws, ierr)
  call MPI_Win_free(wd, ierr)

  call MPI_Comm_free(merged, ierr)
  call MPI_Comm_free(inter, ierr)
  call MPI_Comm_free(dga, ierr)
  call MPI_Comm_free(dg, ierr)
  call MPI_Comm_free(sub, ierr)
  call MPI_Comm_free(grid, ierr)
  call MPI_Comm_free(ring, ierr)
  call MPI_Comm_free(cgroup, ierr)
  call MPI_Comm_free(created, ierr)
  call MPI_Comm_free(shm, ierr)
  call MPI_Comm_free(split, ierr)
  call MPI_Comm_free(dup, ierr)
  call MPI_Group_free(wgroup, ierr)

  if (me == 0) print '(a,i0,a)', 'mpi_coverage_validate_f: all self-checks passed on ', n, ' ranks'
  call MPI_Finalize(ierr)

contains

  subroutine improbe_once(f)
    integer, intent(out) :: f
    logical :: lf
    call MPI_Improbe(left, 3, MPI_COMM_WORLD, lf, msg, MPI_STATUS_IGNORE, ierr)
    f = merge(1, 0, lf)
  end subroutine

  subroutine expect(what, got_v, want)
    character(len=*), intent(in) :: what
    double precision, intent(in) :: got_v, want
    if (got_v /= want) then
      print '(a,i0,a,a,a,g0,a,g0)', 'rank ', me, ': ', what, ': got ', got_v, ', expected ', want
      call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
    end if
  end subroutine
end program
