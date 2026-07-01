#ifndef SYMBOLIZER_H
#define SYMBOLIZER_H

#include "../include/miniprof.h"

void symbolizer_init(void);
void symbolizer_resolve(miniprof_sym_t *syms, int n, int use_addr2line);
int  symbolizer_quick_name(void *pc, char *out, int outlen);

#endif
