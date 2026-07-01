#ifndef MPA_WRITER_H
#define MPA_WRITER_H

/*
 * Write MPA (miniprof Analysis) suggestions to `path`.
 *
 * names[i] / pcts[i] / groups[i] are parallel arrays of length n,
 * describing each function's name, aggregate Samp%, and group
 * (USER/MPI/OMP/LIBC/SYSTEM/ETC). Only USER and MPI functions whose
 * pcts[i] >= threshold_pct are written.
 */
void mpa_write_suggestions(const char *path,
                           const char **names, const double *pcts,
                           const char **groups, int n,
                           double threshold_pct);

#endif
