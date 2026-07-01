#ifndef MPI_TRACE_H
#define MPI_TRACE_H

void mpi_trace_set_enabled(int enabled);
void mpi_trace_iterate(void (*cb)(const char *name, long calls,
                                  double total_s, long bytes, void *arg),
                       void *arg);

#endif
