#ifndef MINIPROF_IO_TRACE_H
#define MINIPROF_IO_TRACE_H

/*
 * io_trace.h — POSIX and MPI-IO interposition for snapshot mode
 *
 * Counts calls, bytes and time for application file I/O. Only file
 * descriptors that were opened through an intercepted open()/fopen() call
 * on a regular path are tracked, so sockets, pipes, the terminal and
 * /proc, /sys and /dev reads (including miniprof's own) never count.
 * POSIX I/O issued from inside an MPI or MPI-IO call (the MPI library's
 * own back end) is excluded via g_miniprof_tool_depth; MPI-IO is counted
 * separately at the MPI_File_* level instead.
 */

#include <stdint.h>

typedef struct {
    uint64_t open_calls, close_calls, read_calls, write_calls, sync_calls;
    uint64_t read_bytes, write_bytes;
    uint64_t read_ns, write_ns, meta_ns;   /* meta = open/close/sync */
} io_counters_t;

typedef struct {
    io_counters_t posix;
    io_counters_t mpiio;
} io_snap_t;

void io_trace_set_enabled(int enabled);  /* resets counters when enabling */
void io_trace_get(io_snap_t *out);

#endif
