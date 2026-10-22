/* Stack footprint and trim-policy benchmarks.
 *
 * The goroutine pool's stack footprint is the trade-off this file
 * measures: the trim advice decides whether a recycled goroutine's
 * pages go back to the kernel (cheap to recycle, expensive in resident
 * memory) or stay (the other way round).  The two numbers are coupled,
 * so they are measured together: the latency of a steady-state spawn
 * and the resident set it leaves behind, per policy.
 *
 * The policy is selected with ui_SetStackTrimPolicy() /
 * ui_SetStackMemoryLimit(); a benchmark that only used the environment
 * variables would measure whatever the caller happened to export.
 */
#include "ui.h"
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Reading the process footprint ── */

/* Resident set in MiB, from /proc/self/status. */
static double
rss_mib(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0.0;
    char line[256];
    double kb = 0.0;
    while (fgets(line, sizeof(line), f))
    {
        if (!strncmp(line, "VmRSS:", 6))
        {
            kb = strtod(line + 6, NULL);
            break;
        }
    }
    fclose(f);
    return kb / 1024.0;
}

/* Number of VMAs (the lines of /proc/self/maps). */
static long
vma_count(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    char line[512];
    long n = 0;
    while (fgets(line, sizeof(line), f))
        n++;
    fclose(f);
    return n;
}

/* ── The steady-state workload ──
 *
 * Spawn a wave of goroutines, wait for them to finish, repeat.  The
 * finished goroutines go back to the pool, so the next wave reuses
 * their stacks - this is the path the trim policy governs.  One warmup
 * wave runs before the measurement window opens, so the one-off cost
 * of the first wave (fresh stacks: mmap, mprotect, first-touch faults)
 * does not get amortised into the reported per-op cost.
 */

#define WAVE 256

static int           st_done;
static int           st_waves;
static ui_bench_ctx *st_ctx;
static int           st_deep_depth;   /* 0 = shallow worker */

static volatile char st_sink;

/* A goroutine that recurses `depth` frames deep and returns.  The
 * writes are volatile rather than a memset: at -O3 the compiler can see
 * that only buf[0] is ever read and shrink the touch to one byte, which
 * would make this measure a single page of stack. */
static void
st_deep(int depth)
{
    volatile char buf[4096];
    for (int i = 0; i < 4096; i += 64)
        buf[i] = (char)(depth + i);
    if (depth > 0)
        st_deep(depth - 1);
    st_sink = buf[depth & 4095];
}

static void
st_child(void)
{
    if (st_deep_depth > 0)
        st_deep(st_deep_depth);
    __sync_fetch_and_add(&st_done, 1);
}

static void
st_wave(void)
{
    int target = __atomic_load_n(&st_done, __ATOMIC_RELAXED) + WAVE;
    for (int i = 0; i < WAVE; i++)
        ui_Go(st_child);
    while (__atomic_load_n(&st_done, __ATOMIC_RELAXED) < target)
        ui_Yield();
}

static void
st_root(void)
{
    st_wave();                      /* warmup, outside the window */
    ui_bench_start(st_ctx);
    for (int w = 0; w < st_waves; w++)
        st_wave();
    ui_bench_stop(st_ctx);
}

/* Run the workload; returns the elapsed nanoseconds per goroutine. */
static double
run_workload(ui_bench_ctx *b, int ops)
{
    int waves = ops / WAVE;
    if (waves < 1)
        waves = 1;
    st_done = 0;
    st_waves = waves;
    st_ctx = b;
    ui_Go(st_root);
    ui_Run();
    return (double)(b->t1 - b->t0) / (double)(waves * WAVE);
}

/* ── The configuration each benchmark runs under ── */

typedef struct {
    ui_StackTrimPolicy policy;
    int                budget_set;
    size_t             budget;
} st_cfg;

static void
apply_cfg(const st_cfg *cfg)
{
    ui_SetStackTrimPolicy(cfg->policy);
    if (cfg->budget_set)
        ui_SetStackMemoryLimit(cfg->budget);
    else
        ui_SetStackMemoryLimit(0);
}

/* Steady-state spawn cost under a given trim policy. */
static void
bench_warm_policy(ui_bench_ctx *b, int n)
{
    apply_cfg((const st_cfg *)b->arg);
    ui_Init();
    run_workload(b, n);
    ui_bench_set_ops(b, n);
    ui_Fini();
}

/* Resident set left behind by the pool under a given trim policy. */
static void
bench_rss_policy(ui_bench_ctx *b, int n)
{
    apply_cfg((const st_cfg *)b->arg);
    ui_Init();
    double ns = run_workload(b, n);
    /* Read the footprint while the pool still holds the stacks: the
     * teardown would give the pages back and the number would be
     * meaningless. */
    double mib = rss_mib();
    ui_bench_set_ops(b, n);
    ui_bench_report(b, mib, "MiB");
    ui_bench_report_extra(b, "spawn", ns, "ns/op");
    ui_Fini();
}

/* VMAs created by a batch of goroutines.
 *
 * Every goroutine stack costs about two VMAs (the arena is PROT_NONE
 * and the slot's guard page splits it in two), and the kernel's
 * vm.max_map_count is what actually caps the number of goroutines that
 * can be alive at the same time.  The count is read while the
 * goroutines are still queued, so their stacks are mapped but their
 * pages are not touched yet.
 *
 * The absolute count is the meaningful number here; the "two VMAs per
 * goroutine" invariant is a white-box assertion and lives in the test
 * suite, where the pre-allocated pool size is known.
 */
static volatile int vma_release;

static void
vma_blocker(void)
{
    while (!vma_release)
        ui_Sleep(20);
}

static void
bench_vma_count(ui_bench_ctx *b, int n)
{
    if (n < 1)
        n = 1;
    ui_Init();
    long before = vma_count();
    for (int i = 0; i < n; i++)
        ui_Go(vma_blocker);
    long after = vma_count();
    vma_release = 1;
    ui_Run();
    ui_bench_set_ops(b, n);
    ui_bench_report(b, (double)after, "count");
    ui_bench_report_extra(b, "vma_delta", (double)(after - before), "count");
    ui_bench_report_extra(b, "rss", rss_mib(), "MiB");
    ui_Fini();
}

/* The same measurements with a goroutine that uses ~64KB of stack.
 *
 * The trim can only return the pages a goroutine was *seen* to use: the
 * low-water mark is sampled when the goroutine switches out, so a
 * goroutine that recurses deeply and then returns leaves no trace of
 * its depth.  The resident set of the pool is then driven by the peak
 * stack depth, not by the live set, and no trim policy changes that.
 * These two metrics are what make that visible. */
#define DEEP_FRAMES 16

static void
bench_warm_deep(ui_bench_ctx *b, int n)
{
    apply_cfg((const st_cfg *)b->arg);
    st_deep_depth = DEEP_FRAMES;
    ui_Init();
    run_workload(b, n);
    ui_bench_set_ops(b, n);
    st_deep_depth = 0;
    ui_Fini();
}

static void
bench_rss_deep(ui_bench_ctx *b, int n)
{
    apply_cfg((const st_cfg *)b->arg);
    st_deep_depth = DEEP_FRAMES;
    ui_Init();
    double ns = run_workload(b, n);
    double mib = rss_mib();
    ui_bench_set_ops(b, n);
    ui_bench_report(b, mib, "MiB");
    ui_bench_report_extra(b, "spawn", ns, "ns/op");
    st_deep_depth = 0;
    ui_Fini();
}

/* ── Main ── */

static const st_cfg cfg_free    = { UI_STACK_TRIM_FREE, 0, 0 };
static const st_cfg cfg_strict  = { UI_STACK_TRIM_DONTNEED, 0, 0 };
static const st_cfg cfg_keep    = { UI_STACK_TRIM_NONE, 0, 0 };
static const st_cfg cfg_budget  = { UI_STACK_TRIM_FREE, 1, 4 * 1024 * 1024 };

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI stack footprint benchmarks ===\n\n");

    /* The same workload under each policy, so the numbers can be read
     * against each other. */
    ui_bench_add("bench_stack", "goroutine spawn+exit (warm, MADV_FREE)",
                 bench_warm_policy, (void *)&cfg_free, 50000);
    ui_bench_add("bench_stack", "goroutine spawn+exit (warm, MADV_DONTNEED)",
                 bench_warm_policy, (void *)&cfg_strict, 50000);
    ui_bench_add("bench_stack", "goroutine spawn+exit (warm, keep pages)",
                 bench_warm_policy, (void *)&cfg_keep, 50000);
    ui_bench_add("bench_stack", "goroutine spawn+exit (warm, 4MB budget)",
                 bench_warm_policy, (void *)&cfg_budget, 50000);

    ui_bench_add("bench_stack", "stack pool RSS (MADV_FREE)", bench_rss_policy,
                 (void *)&cfg_free, 50000);
    ui_bench_add("bench_stack", "stack pool RSS (MADV_DONTNEED)",
                 bench_rss_policy, (void *)&cfg_strict, 50000);
    ui_bench_add("bench_stack", "stack pool RSS (keep pages)", bench_rss_policy,
                 (void *)&cfg_keep, 50000);

    ui_bench_add("bench_stack", "VMA count (1000 goroutines)", bench_vma_count,
                 NULL, 1000);

    ui_bench_add("bench_stack", "goroutine spawn+exit (deep stack, 64KB)",
                 bench_warm_deep, (void *)&cfg_free, 50000);
    ui_bench_add("bench_stack", "stack pool RSS (deep stack, DONTNEED)",
                 bench_rss_deep, (void *)&cfg_strict, 50000);

    int rc = ui_bench_main(argc, argv);
    printf("\n=== Done ===\n");
    return rc;
}
