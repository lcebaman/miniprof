#ifndef MINIPROF_JSON_WRITER_H
#define MINIPROF_JSON_WRITER_H

/*
 * json_writer.h — minimal streaming JSON emitter (no allocation, no deps)
 *
 * Used by snapshot mode to write the machine-readable fingerprint that
 * every downstream report (text verdict, HTML, diff) is generated from.
 * Streaming rather than building a DOM: the writer only tracks nesting
 * depth and whether the current container already has an element, so
 * output size is unbounded while memory use is a fixed few hundred bytes.
 *
 * Every value-emitting call takes an optional `key`: pass a string inside
 * an object, NULL inside an array (or for the top-level value).
 *
 * Non-finite doubles (NaN/Inf) are written as `null` — JSON has no
 * representation for them, and a metric that couldn't be computed is
 * exactly what `null` means in the snapshot schema.
 */

#include <stdio.h>
#include <stdint.h>

#define JW_MAX_DEPTH 32

typedef struct {
    FILE *f;
    int   depth;
    int   has_elem[JW_MAX_DEPTH]; /* 1 once the container at this depth has an element */
    int   error;                  /* sticky: set on overflow/underflow misuse            */
} jw_t;

void jw_init(jw_t *w, FILE *f);

void jw_begin_obj(jw_t *w, const char *key);
void jw_end_obj  (jw_t *w);
void jw_begin_arr(jw_t *w, const char *key);
void jw_end_arr  (jw_t *w);

void jw_str (jw_t *w, const char *key, const char *val); /* NULL val → null */
void jw_int (jw_t *w, const char *key, int64_t val);
void jw_dbl (jw_t *w, const char *key, double val);
void jw_bool(jw_t *w, const char *key, int val);
void jw_null(jw_t *w, const char *key);

/* Finish the document (trailing newline). Returns 0 if the document was
 * well-formed (all containers closed, no misuse), -1 otherwise. */
int  jw_finish(jw_t *w);

#endif /* MINIPROF_JSON_WRITER_H */
