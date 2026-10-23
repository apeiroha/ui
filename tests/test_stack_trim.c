/* Stack trim policy: numeric assertions.
 *
 * tests/test_stack_policy.c covers the public knobs (the setters and
 * the budget).  This file covers what the runtime actually *does* with
 * them, in numbers that must not drift:
 *
 *   - which madvise advice each policy produces,
 *   - how many madvise calls a recycle costs,
 *   - that the trim never touches more than the committed region (the
 *     low-water mark bounds it),
 *   - that MADV_DONTNEED gives the pages back right away while
 *     MADV_FREE only marks them reclaimable.
 *
 * The last one is the interesting distinction: MADV_DONTNEED unmaps the
 * pages at once, MADV_FREE leaves them resident until the kernel needs
 * the memory.  The resident set is what tells the two apart - a minor
 * fault count does not, because a write to a lazily freed page counts
 * as a fault too and is much cheaper than a real page allocation.
 *
 * The advice is observed through a UI_DEBUG_STACK_TRIM hook, so the
 * release build is unaffected.
 */
#include "ui.h"
#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-56s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-56s ... FAIL\n", name); failed++; }
}

/* Resident set in kB, from /proc/self/status. */
static long
rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (!strncmp(line, "VmRSS:", 6))
        {
            kb = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static int spawn_done;

static void
worker(void)
{
    __sync_fetch_and_add(&spawn_done, 1);
}

/* One spawn+exit: the goroutine comes out of the pre-allocated pool,
 * runs and is recycled, which is where the trim happens. */
static void
one_spawn(void)
{
    spawn_done = 0;
    ui_Go(worker);
    ui_Run();
}

/* ── The advice each policy produces ── */

static void
check_advice(ui_StackTrimPolicy policy, int want_calls, int want_advice,
             const char *name)
{
    ui_SetStackTrimPolicy(policy);
    ui_SetStackMemoryLimit(0);
    ui_debug_trim_calls = 0;
    ui_debug_trim_advice = -1;

    if (ui_Init() != 0)
    {
        check(0, name);
        return;
    }
    one_spawn();
    ui_Fini();

    check(ui_debug_trim_calls == want_calls &&
              (want_calls == 0 || ui_debug_trim_advice == want_advice),
          name);
}

/* ── The trim never reaches past the committed region ── */

static void
check_trim_bound(const char *name)
{
    ui_SetStackTrimPolicy(UI_STACK_TRIM_DONTNEED);
    ui_SetStackMemoryLimit(0);
    ui_debug_trim_calls = 0;
    ui_debug_trim_len = 0;
    ui_debug_stack_committed = 0;

    if (ui_Init() != 0)
    {
        check(0, name);
        return;
    }
    one_spawn();
    ui_Fini();

    check(ui_debug_trim_len > 0 &&
              ui_debug_trim_len <= ui_debug_stack_committed,
          name);
}

/* ── Do the pages go back, and when? ── */

#define DEEP_LEVELS 128            /* 128 * 4KB = 512KB of stack */
#define DEEP_STACK  (1024 * 1024)

static volatile char sink;

/* A goroutine that uses half a megabyte of stack and then returns,
 * unwinding its frames on the way out.
 *
 * The trim must not depend on the goroutine being *seen* at its
 * deepest: the low-water mark is sampled when it switches out, and by
 * then the deep frames are gone.  An earlier version bounded the trim
 * with that mark, which left deep goroutines' pages resident forever
 * (measured: 364MB resident for a deep workload under MADV_DONTNEED,
 * the "lowest resident set" policy).  This workload has no yield while
 * deep on purpose - it is the regression test for that. */
static void
touch_stack(int depth)
{
    volatile char buf[4096];
    /* Volatile writes, not memset: at -O3 the compiler can see that only
     * buf[0] is ever read and shrink the touch to a single byte, which
     * would make the whole test measure one page of stack. */
    for (int i = 0; i < 4096; i += 64)
        buf[i] = (char)(depth + i);
    if (depth == 0)
    {
        sink = buf[0];
        return;
    }
    touch_stack(depth - 1);
    sink = buf[depth & 4095];      /* used after the call: no tail call */
}

static void
deep_worker(void)
{
    touch_stack(DEEP_LEVELS);
}

/* Run the deep workload and report the resident set afterwards.
 *
 * Several goroutines, not one: a single stack's pages are a rounding
 * error in the process RSS, and the point is what the *pool* keeps. */
#define DEEP_N 64

static long
resident_after_deep_run(void)
{
    if (ui_Init() != 0)
        return -1;
    spawn_done = 0;
    for (int i = 0; i < DEEP_N; i++)
        ui_GoSized(deep_worker, DEEP_STACK);
    ui_Run();
    long after = rss_kb();
    ui_Fini();
    return after;
}

static void
check_resident_deep(ui_StackTrimPolicy policy, int expect_returned,
                    const char *name)
{
    ui_SetStackTrimPolicy(UI_STACK_TRIM_NONE);
    ui_SetStackMemoryLimit(0);
    long kept_after = resident_after_deep_run();

    ui_SetStackTrimPolicy(policy);
    ui_SetStackMemoryLimit(0);
    long after = resident_after_deep_run();

    if (after < 0 || kept_after < 0)
    {
        check(0, name);
        return;
    }
    /* DEEP_N goroutines * 512KB of touched stack, in kB. */
    long deep_kb = (long)DEEP_N * (DEEP_LEVELS * 4);
    long kept = kept_after > after ? kept_after - after : 0;
    if (expect_returned)
        check(kept >= deep_kb / 2, name);
    else
        check(kept < deep_kb / 2, name);
}

static void
check_resident(ui_StackTrimPolicy policy, int expect_returned, const char *name)
{
    /* The reference: a run that keeps its pages.  The difference
     * between the two resident sets is what the policy gave back. */
    ui_SetStackTrimPolicy(UI_STACK_TRIM_NONE);
    ui_SetStackMemoryLimit(0);
    long kept_after = resident_after_deep_run();

    ui_SetStackTrimPolicy(policy);
    ui_SetStackMemoryLimit(0);
    long after = resident_after_deep_run();

    if (after < 0 || kept_after < 0)
    {
        check(0, name);
        return;
    }
    long kept = kept_after > after ? kept_after - after : 0;

    if (expect_returned)
        check(kept >= 128, name);
    else
        check(kept < 128, name);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI stack trim policy (numeric) tests ===\n");

#ifndef UI_DEBUG_STACK_TRIM
    printf("  TEST: built without UI_DEBUG_STACK_TRIM ... FAIL\n");
    printf("\n=== Results: 0 passed, 1 failed ===\n");
    return 1;
#else
    check_advice(UI_STACK_TRIM_NONE, 0, -1,
                 "UI_STACK_TRIM_NONE performs no madvise at all");
    check_advice(UI_STACK_TRIM_FREE, 1, MADV_FREE,
                 "UI_STACK_TRIM_FREE asks for MADV_FREE");
    check_advice(UI_STACK_TRIM_DONTNEED, 1, MADV_DONTNEED,
                 "UI_STACK_TRIM_DONTNEED asks for MADV_DONTNEED");
    check_trim_bound("the trim never reaches past the committed region");

    /* The pages: MADV_DONTNEED unmaps them at once, so they are gone
     * from the resident set right after the recycle.  MADV_FREE only
     * marks them reclaimable - the kernel gives them back when it
     * needs the memory, not before - so they are still resident. */
    check_resident(UI_STACK_TRIM_DONTNEED, 1,
                   "MADV_DONTNEED returns the pages right away");
    check_resident(UI_STACK_TRIM_FREE, 0,
                   "MADV_FREE keeps the pages resident until pressure");
    check_resident(UI_STACK_TRIM_NONE, 0,
                   "keeping the pages returns nothing to the kernel");

    /* The deep goroutine's pages are returned too.  This is the
     * regression test for the low-water bound: the goroutine recurses
     * to 512KB and returns without ever yielding while deep, so a trim
     * driven by "the deepest address we have seen" would leave every
     * one of those pages resident. */
    check_resident_deep(UI_STACK_TRIM_DONTNEED, 1,
                        "a goroutine that recursed deep has its pages returned");

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
#endif
}
