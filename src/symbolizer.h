#ifndef SYMBOLIZER_H
#define SYMBOLIZER_H

#include "../include/miniprof.h"

void symbolizer_init(void);
void symbolizer_resolve(miniprof_sym_t *syms, int n, int use_addr2line);
int  symbolizer_quick_name(void *pc, char *out, int outlen);
/* For the PCs with want[i] set, replace the symbol-table name with the
 * innermost (possibly inlined) function from the DSO's debug information,
 * when it has any: one addr2line process per such DSO, addresses on
 * stdin. Returns how many names changed. */
int  symbolizer_inline_names(miniprof_sym_t *syms, const char *want, int n);

#endif
