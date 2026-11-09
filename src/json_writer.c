/*
 * json_writer.c — minimal streaming JSON emitter. See json_writer.h.
 */

#include "json_writer.h"
#include <math.h>
#include <inttypes.h>

void jw_init(jw_t *w, FILE *f)
{
    w->f = f;
    w->depth = 0;
    w->error = 0;
    for (int i = 0; i < JW_MAX_DEPTH; i++) w->has_elem[i] = 0;
}

static void indent(jw_t *w)
{
    for (int i = 0; i < w->depth; i++) fputs("  ", w->f);
}

static void write_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n",  f); break;
        case '\r': fputs("\\r",  f); break;
        case '\t': fputs("\\t",  f); break;
        case '\b': fputs("\\b",  f); break;
        case '\f': fputs("\\f",  f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else           fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* Separator + indentation + optional key, before any value. */
static void prefix(jw_t *w, const char *key)
{
    if (w->depth > 0) {
        if (w->has_elem[w->depth]) fputc(',', w->f);
        fputc('\n', w->f);
        w->has_elem[w->depth] = 1;
        indent(w);
    }
    if (key) {
        write_escaped(w->f, key);
        fputs(": ", w->f);
    }
}

static void open_container(jw_t *w, const char *key, char c)
{
    prefix(w, key);
    fputc(c, w->f);
    if (w->depth + 1 >= JW_MAX_DEPTH) { w->error = 1; return; }
    w->depth++;
    w->has_elem[w->depth] = 0;
}

static void close_container(jw_t *w, char c)
{
    if (w->depth <= 0) { w->error = 1; return; }
    int had = w->has_elem[w->depth];
    w->depth--;
    if (had) { fputc('\n', w->f); indent(w); }
    fputc(c, w->f);
}

void jw_begin_obj(jw_t *w, const char *key) { open_container(w, key, '{'); }
void jw_end_obj  (jw_t *w)                  { close_container(w, '}'); }
void jw_begin_arr(jw_t *w, const char *key) { open_container(w, key, '['); }
void jw_end_arr  (jw_t *w)                  { close_container(w, ']'); }

void jw_str(jw_t *w, const char *key, const char *val)
{
    prefix(w, key);
    if (val) write_escaped(w->f, val);
    else     fputs("null", w->f);
}

void jw_int(jw_t *w, const char *key, int64_t val)
{
    prefix(w, key);
    fprintf(w->f, "%" PRId64, val);
}

void jw_dbl(jw_t *w, const char *key, double val)
{
    prefix(w, key);
    if (isfinite(val)) fprintf(w->f, "%.12g", val);
    else               fputs("null", w->f);
}

void jw_bool(jw_t *w, const char *key, int val)
{
    prefix(w, key);
    fputs(val ? "true" : "false", w->f);
}

void jw_null(jw_t *w, const char *key)
{
    prefix(w, key);
    fputs("null", w->f);
}

int jw_finish(jw_t *w)
{
    fputc('\n', w->f);
    return (w->error || w->depth != 0) ? -1 : 0;
}
