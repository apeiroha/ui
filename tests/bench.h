/* tests/bench.h — a Go-style benchmark harness for the ui runtime.
 *
 * The old harness printed one number per benchmark: a single ns/op taken
 * from a fixed iteration count.  On a shared runner that number is noise -
 * you cannot tell a 5% regression from machine drift, and the "min of 3"
 * the collector applied hid the distribution entirely.
 *
 * This harness follows Go's testing.B instead:
 *
 *   - the iteration count is chosen by the runner, not hard-coded, so
 *     every measurement covers the same wall-clock budget and the
 *     per-operation cost is not an artefact of an arbitrary N,
 *   - a warmup round is discarded,
 *   - the measurement is repeated and every sample is kept, so the
 *     report can show a median, a spread and a stability figure,
 *   - the timing is nanosecond-resolution and the timing window is
 *     explicit, so setup (ui_Init/ui_Fini) stays out of the numbers.
 *
 * A benchmark body looks like this:
 *
 *     static void bench_spawn(ui_bench_ctx *b, int n) {
 *         ui_Init();
 *         ui_bench_start(b);
 *         for (int i = 0; i < n; i++) ui_Go(worker);
 *         ui_Run();
 *         ui_bench_stop(b);
 *         ui_bench_set_ops(b, n);          // default is n
 *         ui_Fini();
 *     }
 *
 * A body may report a primary metric other than ns/op (RSS, a count,
 * a throughput) with ui_bench_report(), and may attach named extras
 * with ui_bench_report_extra().
 */
#ifndef UI_BENCH_H
#define UI_BENCH_H

#include <stdint.h>

#define UI_BENCH_MAX_METRICS 64
#define UI_BENCH_MAX_SAMPLES 128
#define UI_BENCH_MAX_EXTRA   8

/* Run the body once, no repeats.  For benchmarks whose side effects
 * cannot be replayed (spawning a million goroutines). */
#define UI_BENCH_ONCE  0x1

/* Per-sample state handed to a benchmark body. */
typedef struct {
    void       *arg;       /* the argument the benchmark was registered with */
    uint64_t    t0;        /* set by ui_bench_start() */
    uint64_t    t1;        /* set by ui_bench_stop()  */
    uint64_t    bytes;     /* throughput denominator, optional */
    int         ops;       /* real op count; defaults to n */
    int         has_value; /* a primary metric was reported */
    double      value;
    const char *unit;
    int         nextra;
    const char *extra_key[UI_BENCH_MAX_EXTRA];
    double      extra_value[UI_BENCH_MAX_EXTRA];
    const char *extra_unit[UI_BENCH_MAX_EXTRA];
} ui_bench_ctx;

/* A benchmark body.  `n` is the op count the runner wants. */
typedef void (*ui_bench_fn)(ui_bench_ctx *b, int n);

/* Register a benchmark.
 *
 * `ops` is the op count the benchmark runs.  It is part of the metric's
 * definition, not a tuning knob: a benchmark whose cost has a large
 * one-off component (the first wave of goroutines needs fresh stacks,
 * ~25ms) would otherwise report a per-op cost that depends on how many
 * ops the runner happened to pick, and a comparison between two builds
 * measured at different sizes would be biased.  Pass 0 to let the
 * runner pick the count (--time budget), which is only sensible for
 * benchmarks with no significant fixed cost. */
void ui_bench_add(const char *group, const char *name, ui_bench_fn fn, void *arg,
                  int ops);
void ui_bench_add_ex(const char *group, const char *name, ui_bench_fn fn, void *arg,
                     int flags, int ops);

/* The measurement window.  Everything between start and stop is timed;
 * setup and teardown should sit outside it. */
void ui_bench_start(ui_bench_ctx *b);
void ui_bench_stop(ui_bench_ctx *b);

/* Override the op count when it differs from n (an I/O benchmark whose
 * read count was clamped by the socket buffer, say). */
void ui_bench_set_ops(ui_bench_ctx *b, int ops);

/* Report the primary metric instead of ns/op. */
void ui_bench_report(ui_bench_ctx *b, double value, const char *unit);

/* Attach a named extra metric (reported next to the primary one). */
void ui_bench_report_extra(ui_bench_ctx *b, const char *key, double value,
                           const char *unit);

/* Run everything.  Command line:
 *
 *   --count N        measured repetitions per benchmark (default 6)
 *   --time MS        target duration of one measurement (default 50)
 *   --json PATH      also write the results as JSON
 *   --filter STR     only run benchmarks whose "group/name" contains STR
 *   --only G/N       run exactly one benchmark ("group/name")
 *   --probe N        force the op count of every benchmark to N
 *   --list           list the registered benchmarks and exit
 */
int ui_bench_main(int argc, char **argv);

#endif /* UI_BENCH_H */
