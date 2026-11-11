#ifndef MINIPROF_NODEMON_H
#define MINIPROF_NODEMON_H

/*
 * nodemon.h — node-level (socket-wide) memory and energy counters (P5)
 *
 * Memory-controller, data-fabric and RAPL counters count for a whole
 * socket and perf only opens them system-wide, which needs
 * perf_event_paranoid <= 0 or CAP_PERFMON. miniprof keeps the application
 * unprivileged: one rank per node starts a small helper,
 * `miniprof-nodemon` (src/nodemon.c), that alone holds CAP_PERFMON
 *     sudo setcap cap_perfmon+ep ./miniprof-nodemon
 * opens a fixed set of events, drops the capability, samples them and
 * hands back totals over a pipe. No helper, no capability → the memory
 * block says why and the rest of the snapshot is unaffected.
 *
 * What is counted, per socket (AMD Zen 4 / Zen 5):
 *   - DRAM reads/writes per memory channel. Zen 5: memory-controller
 *     CAS commands (amd_umc_N, one PMU per channel, 64 B each). Zen 4:
 *     data-fabric DRAM-channel beats (64 B), because there the amd_umc
 *     PMUs are DDR5 sub-channels (a line = two CAS of 32 B on a PMU pair)
 *     and the kernel exposes only 12 per socket: 6 of the 12 channels,
 *     NUMA nodes 0,1,4,5 of 8 in NPS4 (tools/diag_umc_coverage.sh). perf's
 *     umc_mem_*_bandwidth metrics there read 2x on half the channels and
 *     nothing on the rest.
 *   - data arriving at the socket's cores from the OTHER socket (amd_df,
 *     32 B per beat): with the socket's DRAM reads this gives the share
 *     of reads that crossed sockets (NUMA placement)
 *   - package energy (power/energy-pkg)
 *
 * Line protocol (helper → library, text):
 *   ok sockets=<n> chans=<per socket> interval_ms=<ms>        on start
 *   error <message>                                          and exit
 *   ... library writes "stop\n" (or closes the pipe) ...
 *   result elapsed_ns=<ns> samples=<n> multiplexed=<0|1>
 *   socket <s> rd=<B> wr=<B> remote_in=<B> energy_uj=<uJ> peak_rd=<B/s> peak_wr=<B/s>
 *   chan <s> <c> rd=<B> wr=<B>
 *   coverage <text>                          only when part of the node is counted (Zen 6)
 *   peak node=<B/s>
 *   end
 */

#include <stdint.h>

#define NM_MAX_SOCKETS 8
#define NM_MAX_CHANS   24        /* memory-controller PMUs per socket */

typedef struct {
    uint64_t rd_bytes, wr_bytes;          /* DRAM, from the memory controllers */
    uint64_t remote_in_bytes;             /* data into this socket's cores from another socket */
    uint64_t energy_uj;
    double   peak_rd_bps, peak_wr_bps;    /* best sampling interval        */
    int      nchans;
    uint64_t chan_rd[NM_MAX_CHANS], chan_wr[NM_MAX_CHANS];
} nm_socket_t;

typedef struct {
    int      available;
    char     reason[384];
    int      nsockets;
    int      interval_ms;
    int      samples;
    int      multiplexed;
    double   elapsed_s;
    double   peak_node_bps;               /* sum over sockets, best interval */
    char     coverage[256];               /* non-empty: counters see only part of the node */
    nm_socket_t s[NM_MAX_SOCKETS];
} nm_result_t;

/* Library side (nodemon_client.c). Call on ONE rank per node. */
void nodemon_start(const char *uarch, int verbose);
void nodemon_stop(nm_result_t *out);      /* fills *out; safe if never started */

#endif
