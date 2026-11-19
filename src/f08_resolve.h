#ifndef F08_RESOLVE_H
#define F08_RESOLVE_H
/*
 * f08_resolve.h — the `use mpi_f08` wrappers (generated *_f08.inc) call the
 * MPI library's own f08 entry point of the same name, found on first use
 * with dlsym(RTLD_NEXT) and cached. A missing symbol cannot happen for a
 * call the program makes (it linked against it), so it aborts loudly.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static inline void *f08_resolve(void **slot, const char *name)
{
    void *f = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (f) return f;
    f = dlsym(RTLD_NEXT, name);
    if (!f) {
        fprintf(stderr, "[miniprof] %s: not found in the MPI library\n", name);
        abort();
    }
    __atomic_store_n(slot, f, __ATOMIC_RELEASE);
    return f;
}

#define F08_REAL(sym) ((__typeof__(&sym))f08_resolve(&f08_real_##sym, #sym))

#endif
