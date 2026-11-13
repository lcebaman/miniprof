/* spawn.h — popen()/pclose() for miniprof's helper commands (addr2line, nm)
 *
 * The children run without LD_PRELOAD: inherited, it would load
 * libminiprof (and through it the MPI library and its dependencies) into
 * every shell and addr2line miniprof starts, which costs seconds on a cold
 * network filesystem. posix_spawn (vfork-based in glibc) is also safe in an
 * MPI process with registered memory, where fork() may not be.
 */
#ifndef MINIPROF_SPAWN_H
#define MINIPROF_SPAWN_H
#include <stdio.h>

FILE *mp_popen(const char *cmd);   /* read end of "/bin/sh -c cmd" */
int   mp_pclose(FILE *fp);

#endif
