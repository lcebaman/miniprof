#ifndef INSTRUMENT_H
#define INSTRUMENT_H

/* Load the function name list to trace, from a .mpa-format file.
   Called once at miniprof_init() when MINIPROF_MODE=trace. */
void instrument_load_trace_list(const char *path);

/* Reverse lookup: runtime PC -> traced function name, if it matches. */
int instrument_lookup_name(void *pc, char *out, int outlen);

#endif
