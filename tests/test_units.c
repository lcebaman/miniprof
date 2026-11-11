/*
 * test_units.c — MPI-free unit tests for snapshot-mode building blocks
 *
 * Built with the host compiler against the individual source files (no
 * libminiprof.so, no MPI), so it runs anywhere in well under a second:
 *
 *   - sysinfo_classify_uarch(): CPUID family/model → uarch label for the
 *     parts the event tables will key on (Genoa, Turin, GNR, ...), and
 *     "unknown stays unknown" for unrecognised IDs.
 *   - json_writer: nesting, commas, escaping, non-finite → null, and
 *     misuse detection. The emitted document is also written to a file
 *     so `make test-units` can re-parse it with Python's json module.
 *
 * Usage: test_units [json_out_path]
 */

#include "../src/sysinfo.h"
#include "../src/json_writer.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (cond) { printf("PASS: "); }                 \
    else      { printf("FAIL: "); g_fail++; }       \
    printf(__VA_ARGS__); printf("\n");              \
} while (0)

static void expect_uarch(const char *vendor, int fam, int model, const char *want)
{
    char u[32], n[32];
    sysinfo_classify_uarch(vendor, fam, model, u, sizeof(u), n, sizeof(n));
    CHECK(strcmp(u, want) == 0, "uarch(%s, 0x%X, 0x%02X) = '%s' (want '%s'%s%s)",
          vendor, fam, model, u, want, n[0] ? ", " : "", n);
}

static void test_uarch(void)
{
    expect_uarch("AuthenticAMD", 0x17, 0x31, "zen2");           /* Rome         */
    expect_uarch("AuthenticAMD", 0x19, 0x01, "zen3");           /* Milan        */
    expect_uarch("AuthenticAMD", 0x19, 0x11, "zen4");           /* Genoa        */
    expect_uarch("AuthenticAMD", 0x19, 0xA0, "zen4");           /* Bergamo      */
    expect_uarch("AuthenticAMD", 0x1A, 0x02, "zen5");           /* Turin        */
    expect_uarch("AuthenticAMD", 0x1A, 0x11, "zen5");           /* Turin dense  */
    expect_uarch("GenuineIntel", 6,    0xAD, "granite_rapids");
    expect_uarch("GenuineIntel", 6,    0x8F, "sapphire_rapids");
    expect_uarch("GenuineIntel", 6,    0xCF, "emerald_rapids");
    /* unrecognised IDs must stay unclassified, never guessed */
    expect_uarch("AuthenticAMD", 0x1A, 0xF0, "");
    /* Zen 5 / Zen 6 split of family 1Ah, as in Linux bsp_init_amd() */
    expect_uarch("AuthenticAMD", 0x1A, 0x2F, "zen5");
    expect_uarch("AuthenticAMD", 0x1A, 0x44, "zen5");
    expect_uarch("AuthenticAMD", 0x1A, 0x70, "zen5");
    expect_uarch("AuthenticAMD", 0x1A, 0xD7, "zen5");
    expect_uarch("AuthenticAMD", 0x1A, 0x50, "zen6");           /* was misread as Zen 5 */
    expect_uarch("AuthenticAMD", 0x1A, 0x5F, "zen6");
    expect_uarch("AuthenticAMD", 0x1A, 0x80, "zen6");           /* client       */
    expect_uarch("AuthenticAMD", 0x1A, 0xA0, "zen6");
    expect_uarch("AuthenticAMD", 0x1A, 0xC0, "zen6");
    expect_uarch("AuthenticAMD", 0x1A, 0xD8, "zen6");
    expect_uarch("AuthenticAMD", 0x1A, 0xEF, "zen6");
    expect_uarch("AuthenticAMD", 0x1A, 0x30, "");               /* unassigned   */
    expect_uarch("AuthenticAMD", 0x1A, 0xB0, "");
    expect_uarch("GenuineIntel", 6,    0x01, "");
    expect_uarch("SomeVendor",   1,    1,    "");
}

static void test_json(const char *path)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "open %s for JSON round-trip", path);
    if (!f) return;

    jw_t w;
    jw_init(&w, f);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "plain", "hello");
    jw_str(&w, "escaped", "quote\" backslash\\ newline\n tab\t ctrl\x01");
    jw_str(&w, "null_str", NULL);
    jw_int(&w, "big", 1234567890123LL);
    jw_int(&w, "neg", -42);
    jw_dbl(&w, "pi", 3.14159265358979);
    jw_dbl(&w, "nan", NAN);
    jw_dbl(&w, "inf", INFINITY);
    jw_bool(&w, "t", 1);
    jw_bool(&w, "f", 0);
    jw_null(&w, "n");
    jw_begin_arr(&w, "empty_arr");
    jw_end_arr(&w);
    jw_begin_obj(&w, "empty_obj");
    jw_end_obj(&w);
    jw_begin_arr(&w, "arr");
    jw_int(&w, NULL, 1);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "k", "v");
    jw_end_obj(&w);
    jw_begin_arr(&w, NULL);
    jw_dbl(&w, NULL, 0.5);
    jw_end_arr(&w);
    jw_end_arr(&w);
    jw_end_obj(&w);
    int rc = jw_finish(&w);
    fclose(f);
    CHECK(rc == 0, "well-formed document reports success");

    /* misuse: unclosed container must be reported */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        jw_t bad;
        jw_init(&bad, devnull);
        jw_begin_obj(&bad, NULL);
        jw_begin_arr(&bad, "x");
        CHECK(jw_finish(&bad) != 0, "unclosed containers reported as error");

        jw_init(&bad, devnull);
        jw_end_obj(&bad);
        CHECK(jw_finish(&bad) != 0, "close without open reported as error");
        fclose(devnull);
    }
}

int main(int argc, char **argv)
{
    const char *json_path = argc > 1 ? argv[1] : "test_units_out.json";
    test_uarch();
    test_json(json_path);
    printf("\n%s\n", g_fail ? "SOME UNIT TESTS FAILED" : "ALL UNIT TESTS PASSED");
    return g_fail ? 1 : 0;
}
