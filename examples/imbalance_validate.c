/*
 * imbalance_validate.c — known-answer workload for snapshot mode (P1)
 *
 * Compute phase: STEPS iterations; in each, rank r busy-spins for
 * (BASE_MS + r * DELTA_MS) of wall time, then joins an MPI_Allreduce.
 * So, exactly:
 *   compute time of rank r   = STEPS * (BASE_MS + r * DELTA_MS)
 *   Allreduce wait of rank r = STEPS * (N-1-r) * DELTA_MS   (+ latency)
 * from which the expected compute imbalance and MPI time follow.
 * Spinning on the wall clock (not on an iteration count) keeps the
 * answer exact even on an oversubscribed CI runner.
 *
 * I/O phase: a fixed, known byte volume per rank through every
 * interposed path — write/pread on a raw fd, fwrite/fread on a FILE*,
 * fsync, and MPI-IO write_at/read_at on a shared file:
 *   POSIX  write 1.5 MiB (1 MiB write() + 512 KiB fwrite()) + 64000 B text
 *          (fprintf + fputs), 6 open, 6 close, 1 sync
 *   POSIX  read  1.5 MiB (1 MiB pread()  + 512 KiB fread()) + 64000 B text
 *          (fgets + getline + fscanf)
 *   MPI-IO write 256 KiB, read 256 KiB, 1 open, 1 close
 * Data read back is verified, so the run also proves the wrappers pass
 * I/O through intact.
 *
 * Usage: imbalance_validate [STEPS BASE_MS DELTA_MS]   (default 40 10 10)
 * tools/validate_imbalance.py checks the resulting snapshot JSON.
 */

#define _GNU_SOURCE
#include <mpi.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHUNK     (256 * 1024)
#define FCHUNK    (64 * 1024)
#define RAW_BYTES (4 * CHUNK)        /* 1 MiB   */
#define STD_BYTES (8 * FCHUNK)       /* 512 KiB */
#define MPIIO_BYTES CHUNK            /* 256 KiB */
#define TEXT_LINES 2000
#define TEXT_TAIL  "abcdefghijklmnopqrs"  /* "line %06d <tail>\n" = 32 bytes */
#define TEXT_LINE  32

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void spin(double seconds)
{
    double end = now() + seconds;
    volatile double x = 1.0;
    while (now() < end) x = x * 1.0000001 + 1e-9;
}

static void die(int rank, const char *what)
{
    fprintf(stderr, "imbalance_validate rank %d: %s\n", rank, what);
    MPI_Abort(MPI_COMM_WORLD, 1);
}

static void write_all(int fd, const char *buf, size_t n, int rank)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w <= 0) die(rank, "write failed");
        buf += w; n -= (size_t)w;
    }
}

static void io_phase(int rank)
{
    int nprocs;
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    char path[64], mpath[] = "imbalance_validate_mpiio.dat";
    snprintf(path, sizeof(path), "imbalance_validate.%d.dat", rank);

    char *buf = malloc(CHUNK), *chk = malloc(CHUNK);
    if (!buf || !chk) die(rank, "malloc failed");
    for (int i = 0; i < CHUNK; i++) buf[i] = (char)(i * 7 + rank);

    /* raw fd: 1 MiB written, synced */
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) die(rank, "open for write failed");
    for (int c = 0; c < RAW_BYTES / CHUNK; c++) write_all(fd, buf, CHUNK, rank);
    if (fsync(fd) != 0) die(rank, "fsync failed");
    close(fd);

    /* stdio: 512 KiB appended */
    FILE *f = fopen(path, "ab");
    if (!f) die(rank, "fopen for append failed");
    for (int c = 0; c < STD_BYTES / FCHUNK; c++)
        if (fwrite(buf, 1, FCHUNK, f) != FCHUNK) die(rank, "fwrite failed");
    fclose(f);

    /* raw fd: 1 MiB read back with pread, verified */
    fd = open(path, O_RDONLY);
    if (fd < 0) die(rank, "open for read failed");
    for (int c = 0; c < RAW_BYTES / CHUNK; c++) {
        size_t got = 0;
        while (got < CHUNK) {
            ssize_t r = pread(fd, chk + got, CHUNK - got, (off_t)c * CHUNK + got);
            if (r <= 0) die(rank, "pread failed");
            got += (size_t)r;
        }
        if (memcmp(chk, buf, CHUNK) != 0) die(rank, "pread data mismatch");
    }
    close(fd);

    /* stdio: 512 KiB read back, verified */
    f = fopen(path, "rb");
    if (!f) die(rank, "fopen for read failed");
    if (fseek(f, RAW_BYTES, SEEK_SET) != 0) die(rank, "fseek failed");
    for (int c = 0; c < STD_BYTES / FCHUNK; c++) {
        if (fread(chk, 1, FCHUNK, f) != FCHUNK) die(rank, "fread failed");
        if (memcmp(chk, buf, FCHUNK) != 0) die(rank, "fread data mismatch");
    }
    fclose(f);
    unlink(path);

    /* text stdio, as input decks and logs are handled: 2000 lines of
     * exactly TEXT_LINE bytes written with fprintf and fputs, read back
     * with fgets, getline and fscanf, verified */
    f = fopen(path, "w");
    if (!f) die(rank, "fopen for text write failed");
    char line[64];
    for (int k = 0; k < TEXT_LINES / 2; k++)
        if (fprintf(f, "line %06d %s\n", k, TEXT_TAIL) != TEXT_LINE) die(rank, "fprintf failed");
    for (int k = TEXT_LINES / 2; k < TEXT_LINES; k++) {
        snprintf(line, sizeof line, "line %06d %s\n", k, TEXT_TAIL);
        if (fputs(line, f) < 0) die(rank, "fputs failed");
    }
    fclose(f);
    f = fopen(path, "r");
    if (!f) die(rank, "fopen for text read failed");
    char *gl = NULL; size_t gn = 0; int k = 0;
    for (; k < TEXT_LINES / 2; k++) {
        if (!fgets(line, sizeof line, f)) die(rank, "fgets failed");
        if (atoi(line + 5) != k) die(rank, "fgets data mismatch");
    }
    for (; k < 3 * TEXT_LINES / 4; k++) {
        if (getline(&gl, &gn, f) != TEXT_LINE) die(rank, "getline failed");
        if (atoi(gl + 5) != k) die(rank, "getline data mismatch");
    }
    for (; k < TEXT_LINES; k++) {
        int v; char tail[32];
        if (fscanf(f, "line %d %19s\n", &v, tail) != 2 || v != k) die(rank, "fscanf failed");
    }
    free(gl);
    fclose(f);
    unlink(path);

    /* MPI-IO: each rank writes and reads back its own 256 KiB block */
    MPI_File mf;
    if (MPI_File_open(MPI_COMM_WORLD, mpath, MPI_MODE_CREATE | MPI_MODE_RDWR,
                      MPI_INFO_NULL, &mf) != MPI_SUCCESS) die(rank, "MPI_File_open failed");
    MPI_Offset off = (MPI_Offset)rank * MPIIO_BYTES;
    if (MPI_File_write_at(mf, off, buf, MPIIO_BYTES, MPI_BYTE, MPI_STATUS_IGNORE) != MPI_SUCCESS)
        die(rank, "MPI_File_write_at failed");
    memset(chk, 0, MPIIO_BYTES);
    if (MPI_File_read_at(mf, off, chk, MPIIO_BYTES, MPI_BYTE, MPI_STATUS_IGNORE) != MPI_SUCCESS)
        die(rank, "MPI_File_read_at failed");
    if (memcmp(chk, buf, MPIIO_BYTES) != 0) die(rank, "MPI-IO data mismatch");
    MPI_File_close(&mf);
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) unlink(mpath);

    /* the PnetCDF pattern: set_size, a per-rank file view + collective
     * write_all, then a non-blocking write completed with MPI_Wait */
    if (MPI_File_open(MPI_COMM_WORLD, mpath, MPI_MODE_CREATE | MPI_MODE_RDWR,
                      MPI_INFO_NULL, &mf) != MPI_SUCCESS) die(rank, "MPI_File_open (2) failed");
    MPI_File_set_size(mf, (MPI_Offset)2 * nprocs * MPIIO_BYTES);
    MPI_File_set_view(mf, (MPI_Offset)rank * MPIIO_BYTES, MPI_BYTE, MPI_BYTE, "native", MPI_INFO_NULL);
    if (MPI_File_write_all(mf, buf, MPIIO_BYTES, MPI_BYTE, MPI_STATUS_IGNORE) != MPI_SUCCESS)
        die(rank, "MPI_File_write_all failed");
    MPI_File_set_view(mf, 0, MPI_BYTE, MPI_BYTE, "native", MPI_INFO_NULL);
    MPI_Request rq;
    if (MPI_File_iwrite_at(mf, (MPI_Offset)(nprocs + rank) * MPIIO_BYTES, buf, MPIIO_BYTES, MPI_BYTE, &rq)
        != MPI_SUCCESS) die(rank, "MPI_File_iwrite_at failed");
    MPI_Wait(&rq, MPI_STATUS_IGNORE);
    MPI_File_close(&mf);
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) unlink(mpath);

    free(buf);
    free(chk);
}

int main(int argc, char **argv)
{
    int steps = argc > 1 ? atoi(argv[1]) : 40;
    double base_ms = argc > 2 ? atof(argv[2]) : 10.0;
    double delta_ms = argc > 3 ? atof(argv[3]) : 10.0;

    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    /* absorb start-up skew before the measured phase (counted as Barrier) */
    MPI_Barrier(MPI_COMM_WORLD);

    double work = (base_ms + rank * delta_ms) * 1e-3, sum = 0.0;
    for (int s = 0; s < steps; s++) {
        spin(work);
        double v = rank + s, r = 0.0;
        MPI_Allreduce(&v, &r, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        sum += r;
    }

    io_phase(rank);

    double expect = 0.0;
    for (int s = 0; s < steps; s++) expect += nprocs * (nprocs - 1) / 2.0 + (double)nprocs * s;
    if (sum != expect) die(rank, "Allreduce result mismatch");

    if (rank == 0)
        printf("imbalance_validate: %d ranks, %d steps, base %.1f ms, delta %.1f ms: OK\n",
               nprocs, steps, base_ms, delta_ms);
    MPI_Finalize();
    return 0;
}
