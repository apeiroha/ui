/* tests/bench.c — implementation of the Go-style benchmark harness.
 * See tests/bench.h for the model and the command line.
 */
#include "bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UI_BENCH_MAX_N 100000000
#define UI_BENCH_HARNESS "v2"

/* One registered benchmark and the samples it produced. */
typedef struct {
    const char *group;
    const char *name;
    ui_bench_fn fn;
    void       *arg;
    int         flags;
    int         fixed_ops;  /* the benchmark's own op count, 0 = pick one */

    int         nsamples;
    double      samples[UI_BENCH_MAX_SAMPLES];
    int         ops;
    uint64_t    bytes;
    const char *unit;
    double      min, p50, p95, max, mean, stdev, cv_pct;

    int         nextra;
    const char *extra_key[UI_BENCH_MAX_EXTRA];
    double      extra_value[UI_BENCH_MAX_EXTRA];
    const char *extra_unit[UI_BENCH_MAX_EXTRA];
} ui_bench_entry;

static ui_bench_entry g_entries[UI_BENCH_MAX_METRICS];
static int            g_nentries;

/* ── Options ── */
static int         g_opt_count = 6;
static int         g_opt_time_ms = 50;
static const char *g_opt_json;
static const char *g_opt_filter;
static const char *g_opt_only;
static int         g_opt_probe_n;
static int         g_opt_list;

/* ── Registration ── */

void
ui_bench_add_ex(const char *group, const char *name, ui_bench_fn fn, void *arg,
                int flags, int ops)
{
    if (g_nentries >= UI_BENCH_MAX_METRICS)
    {
        fprintf(stderr, "ui_bench: too many benchmarks (max %d)\n",
                UI_BENCH_MAX_METRICS);
        exit(2);
    }
    ui_bench_entry *e = &g_entries[g_nentries++];
    memset(e, 0, sizeof(*e));
    e->group = group;
    e->name = name;
    e->fn = fn;
    e->arg = arg;
    e->flags = flags;
    e->fixed_ops = ops;
    e->unit = "ns/op";
}

void
ui_bench_add(const char *group, const char *name, ui_bench_fn fn, void *arg,
             int ops)
{
    ui_bench_add_ex(group, name, fn, arg, 0, ops);
}

/* ── Body-facing helpers ── */

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void
ui_bench_start(ui_bench_ctx *b)
{
    b->t0 = now_ns();
}

void
ui_bench_stop(ui_bench_ctx *b)
{
    b->t1 = now_ns();
}

void
ui_bench_set_ops(ui_bench_ctx *b, int ops)
{
    if (ops > 0)
        b->ops = ops;
}

void
ui_bench_report(ui_bench_ctx *b, double value, const char *unit)
{
    b->has_value = 1;
    b->value = value;
    b->unit = unit ? unit : "ns/op";
}

void
ui_bench_report_extra(ui_bench_ctx *b, const char *key, double value,
                      const char *unit)
{
    if (b->nextra >= UI_BENCH_MAX_EXTRA)
        return;
    b->extra_key[b->nextra] = key;
    b->extra_value[b->nextra] = value;
    b->extra_unit[b->nextra] = unit ? unit : "";
    b->nextra++;
}

/* ── Running one sample ── */

typedef struct {
    double   value;
    int      ops;
    uint64_t bytes;
    const char *unit;
    int      nextra;
    const char *extra_key[UI_BENCH_MAX_EXTRA];
    double      extra_value[UI_BENCH_MAX_EXTRA];
    const char *extra_unit[UI_BENCH_MAX_EXTRA];
} ui_bench_sample_out;

static void
run_sample(ui_bench_entry *e, int n, ui_bench_sample_out *out)
{
    ui_bench_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arg = e->arg;
    ctx.ops = n;

    uint64_t outer0 = now_ns();
    e->fn(&ctx, n);
    uint64_t outer1 = now_ns();

    uint64_t elapsed;
    if (ctx.t0 != 0 && ctx.t1 >= ctx.t0)
        elapsed = ctx.t1 - ctx.t0;
    else
        elapsed = outer1 - outer0;

    int ops = ctx.ops > 0 ? ctx.ops : n;
    out->ops = ops;
    out->bytes = ctx.bytes;
    if (ctx.has_value)
    {
        out->value = ctx.value;
        out->unit = ctx.unit ? ctx.unit : "ns/op";
    }
    else
    {
        out->value = ops > 0 ? (double)elapsed / (double)ops : 0.0;
        out->unit = "ns/op";
    }
    out->nextra = ctx.nextra;
    for (int i = 0; i < ctx.nextra; i++)
    {
        out->extra_key[i] = ctx.extra_key[i];
        out->extra_value[i] = ctx.extra_value[i];
        out->extra_unit[i] = ctx.extra_unit[i];
    }
}

/* ── Statistics ── */

static int
cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Linear interpolation between order statistics (numpy default). */
static double
percentile(const double *xs, int n, double p)
{
    if (n <= 0)
        return 0.0;
    if (n == 1)
        return xs[0];
    double idx = p * (double)(n - 1);
    int lo = (int)floor(idx);
    int hi = (int)ceil(idx);
    if (lo < 0) lo = 0;
    if (hi >= n) hi = n - 1;
    double frac = idx - (double)lo;
    return xs[lo] + (xs[hi] - xs[lo]) * frac;
}

static void
compute_stats(ui_bench_entry *e)
{
    int n = e->nsamples;
    if (n <= 0)
        return;
    double sorted[UI_BENCH_MAX_SAMPLES];
    memcpy(sorted, e->samples, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);

    e->min = sorted[0];
    e->max = sorted[n - 1];
    e->p50 = percentile(sorted, n, 0.50);
    e->p95 = percentile(sorted, n, 0.95);

    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += sorted[i];
    e->mean = sum / (double)n;
    if (n > 1)
    {
        double acc = 0.0;
        for (int i = 0; i < n; i++)
        {
            double d = sorted[i] - e->mean;
            acc += d * d;
        }
        e->stdev = sqrt(acc / (double)(n - 1));
    }
    else
    {
        e->stdev = 0.0;
    }
    e->cv_pct = e->mean > 0.0 ? e->stdev / e->mean * 100.0 : 0.0;
}

/* ── Driver ── */

/* Should this benchmark run? */
static int
selected(const ui_bench_entry *e)
{
    if (g_opt_only)
    {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", e->group, e->name);
        return strcmp(full, g_opt_only) == 0;
    }
    if (g_opt_filter)
    {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", e->group, e->name);
        return strstr(full, g_opt_filter) != NULL;
    }
    return 1;
}

/* Run one benchmark: pick n, warm up, then take the measured samples. */
static void
run_entry(ui_bench_entry *e)
{
    int once = (e->flags & UI_BENCH_ONCE) != 0;

    int n;
    if (g_opt_probe_n > 0)
        n = g_opt_probe_n;              /* the syscall probe pins N */
    else if (e->fixed_ops > 0)
        n = e->fixed_ops;               /* the benchmark's own size */
    else
        n = 1;

    ui_bench_sample_out out;

    if (!once && e->fixed_ops <= 0 && g_opt_probe_n <= 0)
    {
        /* Auto-scale: grow n until one measurement covers the time
         * budget.  Only for benchmarks that declare no op count of
         * their own - a fixed size is always preferable, because a
         * benchmark with a large one-off cost would report a per-op
         * value that depends on the size the runner happened to pick. */
        uint64_t target = (uint64_t)(g_opt_time_ms > 0 ? g_opt_time_ms : 50)
                          * 1000000ull;
        for (;;)
        {
            uint64_t t0 = now_ns();
            run_sample(e, n, &out);
            uint64_t el = now_ns() - t0;
            if (el >= target || n >= UI_BENCH_MAX_N)
                break;
            double factor = (double)target / (double)(el ? el : 1);
            if (factor < 2.0)
                factor = 2.0;
            double next = (double)n * factor;
            if (next > (double)UI_BENCH_MAX_N)
                next = (double)UI_BENCH_MAX_N;
            n = (int)next + 1;
        }
    }

    /* Warmup rounds at the final size, discarded.  They fault in the
     * pages, fill the pools and let the allocator reach its steady
     * state: without them the first measured sample is systematically
     * slower than the rest (the first spawn batch pays for growing the
     * arena and the heap), which shows up as a trend across the
     * samples rather than as random noise.
     *
     * Neither the syscall probe nor a once-only benchmark warms up: the
     * first measures exactly one round, and the second exists for
     * measurements where a warmup round is a waste of the wall clock
     * (an idle-CPU window, say). */
    int probe = g_opt_probe_n > 0;
    if (!probe && !once)
    {
        for (int w = 0; w < 2; w++)
            run_sample(e, n, &out);
    }

    int reps = (once || probe) ? 1 : g_opt_count;
    for (int i = 0; i < reps && e->nsamples < UI_BENCH_MAX_SAMPLES; i++)
    {
        run_sample(e, n, &out);
        e->samples[e->nsamples++] = out.value;
        e->ops = out.ops;
        e->bytes = out.bytes;
        e->unit = out.unit;
        /* Extras are replaced, not appended: a benchmark that reports
         * "RSS" on every round should end up with one RSS entry, not
         * one per sample. */
        for (int k = 0; k < out.nextra; k++)
        {
            int slot = -1;
            for (int j = 0; j < e->nextra; j++)
            {
                if (!strcmp(e->extra_key[j], out.extra_key[k]))
                {
                    slot = j;
                    break;
                }
            }
            if (slot < 0)
            {
                if (e->nextra >= UI_BENCH_MAX_EXTRA)
                    continue;
                slot = e->nextra++;
                e->extra_key[slot] = out.extra_key[k];
                e->extra_unit[slot] = out.extra_unit[k];
            }
            e->extra_value[slot] = out.extra_value[k];
        }
    }
    compute_stats(e);
}

/* ── Reporting ── */

/* "goroutine spawn+exit" -> "GoroutineSpawnExit" (Go's benchmark names). */
static void
go_name(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    int upper = 1;
    for (const char *p = in; *p && o + 1 < outsz; p++)
    {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
        {
            if (upper && c >= 'a' && c <= 'z')
                c = (unsigned char)(c - 'a' + 'A');
            out[o++] = (char)c;
            upper = 0;
        }
        else
        {
            upper = 1;
        }
    }
    out[o] = '\0';
}

/* Magnitude-aware formatting for the human table.  The values now come
 * in several units (ns/op, MiB, count, us, ms), so a fixed integer
 * column would round the small ones away. */
static void
fmt_value(char *buf, size_t n, double v)
{
    double a = v < 0 ? -v : v;
    if (a >= 100.0)      snprintf(buf, n, "%.0f", v);
    else if (a >= 10.0)  snprintf(buf, n, "%.1f", v);
    else if (a >= 1.0)   snprintf(buf, n, "%.2f", v);
    else                 snprintf(buf, n, "%.3f", v);
}

static void
print_table(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
        ncpu = 1;
    for (int i = 0; i < g_nentries; i++)
    {
        ui_bench_entry *e = &g_entries[i];
        if (!selected(e) || e->nsamples == 0)
            continue;
        char gname[128];
        char val[32], p50[32], p95[32];
        go_name(e->name, gname, sizeof(gname));
        fmt_value(val, sizeof(val), e->p50);
        fmt_value(p50, sizeof(p50), e->p50);
        fmt_value(p95, sizeof(p95), e->p95);
        printf("Benchmark%s-%ld\t%8d\t%10s %s", gname, ncpu, e->ops, val,
               e->unit);
        if (e->nsamples > 1)
            printf("\t(p50 %s  p95 %s  cv %.1f%%  n=%d)",
                   p50, p95, e->cv_pct, e->nsamples);
        if (e->nextra > 0)
        {
            printf("\t[");
            for (int k = 0; k < e->nextra; k++)
            {
                if (k)
                    printf(" ");
                printf("%s=%.2f %s", e->extra_key[k], e->extra_value[k],
                       e->extra_unit[k]);
            }
            printf("]");
        }
        printf("\n");
    }
}

static void
json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        switch (c)
        {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc((char)c, f);   /* UTF-8 passes through */
        }
    }
    fputc('"', f);
}

static void
write_json(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "ui_bench: cannot write %s\n", path);
        return;
    }
    fprintf(f, "{\n  \"harness\": \"" UI_BENCH_HARNESS "\",\n");
    fprintf(f, "  \"benchmarks\": [\n");
    int first = 1;
    for (int i = 0; i < g_nentries; i++)
    {
        ui_bench_entry *e = &g_entries[i];
        if (!selected(e) || e->nsamples == 0)
            continue;
        if (!first)
            fprintf(f, ",\n");
        first = 0;
        fprintf(f, "    {\n");
        fprintf(f, "      \"group\": ");
        json_string(f, e->group);
        fprintf(f, ",\n      \"name\": ");
        json_string(f, e->name);
        fprintf(f, ",\n      \"unit\": ");
        json_string(f, e->unit);
        fprintf(f, ",\n");
        fprintf(f, "      \"value\": %.6g,\n", e->p50);
        fprintf(f, "      \"n\": %d,\n", e->ops);
        fprintf(f, "      \"samples\": [");
        for (int k = 0; k < e->nsamples; k++)
            fprintf(f, "%s%.6g", k ? ", " : "", e->samples[k]);
        fprintf(f, "],\n");
        fprintf(f, "      \"min\": %.6g,\n", e->min);
        fprintf(f, "      \"p50\": %.6g,\n", e->p50);
        fprintf(f, "      \"p95\": %.6g,\n", e->p95);
        fprintf(f, "      \"max\": %.6g,\n", e->max);
        fprintf(f, "      \"mean\": %.6g,\n", e->mean);
        fprintf(f, "      \"stdev\": %.6g,\n", e->stdev);
        fprintf(f, "      \"cv_pct\": %.6g,\n", e->cv_pct);
        fprintf(f, "      \"nsamples\": %d", e->nsamples);
        if (e->nextra > 0)
        {
            fprintf(f, ",\n      \"extra\": {\n");
            for (int k = 0; k < e->nextra; k++)
            {
                fprintf(f, "        ");
                json_string(f, e->extra_key[k]);
                fprintf(f, ": { \"value\": %.6g, \"unit\": ", e->extra_value[k]);
                json_string(f, e->extra_unit[k]);
                fprintf(f, " }%s\n", k + 1 < e->nextra ? "," : "");
            }
            fprintf(f, "      }");
        }
        fprintf(f, "\n    }");
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

/* ── Entry point ── */

int
ui_bench_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--count") && i + 1 < argc)
            g_opt_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc)
            g_opt_time_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc)
            g_opt_json = argv[++i];
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc)
            g_opt_filter = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_opt_only = argv[++i];
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc)
            g_opt_probe_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list"))
            g_opt_list = 1;
        else
        {
            fprintf(stderr, "ui_bench: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (g_opt_count < 1)
        g_opt_count = 1;
    if (g_opt_count > UI_BENCH_MAX_SAMPLES)
        g_opt_count = UI_BENCH_MAX_SAMPLES;

    if (g_opt_list)
    {
        for (int i = 0; i < g_nentries; i++)
            printf("%s/%s\n", g_entries[i].group, g_entries[i].name);
        return 0;
    }

    for (int i = 0; i < g_nentries; i++)
    {
        if (!selected(&g_entries[i]))
            continue;
        /* Progress goes to stderr: stdout carries the result table and
         * must stay machine-readable. */
        fprintf(stderr, "  running %s/%s\n", g_entries[i].group,
                g_entries[i].name);
        run_entry(&g_entries[i]);
    }

    print_table();
    if (g_opt_json)
        write_json(g_opt_json);
    return 0;
}
