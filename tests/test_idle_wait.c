/* Regression tests for the event-driven idle wait.
 *
 * The idle block timeout is a lost-wakeup watchdog, not a poll interval:
 * every real wake source (cross-vCPU wakeup, I/O completion, sleepq
 * deadline) signals directly.  These tests therefore run with a
 * deliberately long block timeout, so any regression back to
 * "wake when the poll times out" shows up as a multi-second latency
 * instead of hiding behind a short tick.
 *
 * Wall-clock assertions, so the bounds are loose enough for a noisy CI
 * box but far below the block timeout the test installs.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a timeout-driven wake costs seconds, short enough
 * that a failing run does not hang the suite for a minute. */
#define BLOCK_MS   2000
#define BOUND_MS   400   /* assertions; ~5% of BLOCK_MS */

static double
now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int g_pass, g_fail;

static void
check(const char *name, bool ok, const char *detail)
{
    printf("  TEST: %s ... %s%s%s\n", name, ok ? "PASS" : "FAIL",
           detail && *detail ? " — " : "", detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
}

/* ── 1. ui_Run() must not wait out the block timeout ──
 *
 * A parked vCPU only re-reads `running` after its block returns, so
 * without the shutdown broadcast ui_Run() pays one full block per
 * parked vCPU.  Observed before the broadcast existed: work finished in
 * ~3.5ms, then the process sat for the whole 100ms block. */

#define MTX_GOROS   50
#define MTX_ITERS   1000

static uint64_t g_mtx;
static volatile int g_val;

static void
mtx_worker(void)
{
    for (int i = 0; i < MTX_ITERS; i++)
    {
        ui_MutexLock(g_mtx);
        g_val++;
        ui_MutexUnlock(g_mtx);
    }
}

static void
test_shutdown_is_prompt(void)
{
    setenv("UI_IDLE_WAIT_MS", "2000", 1);
    if (ui_Init() != 0) exit(1);
    g_mtx = ui_MutexNew();
    g_val = 0;

    double t0 = now_s();
    for (int i = 0; i < MTX_GOROS; i++) ui_Go(mtx_worker);
    ui_Run();
    double dt = (now_s() - t0) * 1000.0;

    char detail[160];
    snprintf(detail, sizeof detail,
             "%d goros x %d ops in %.1f ms (block timeout is %d ms, "
             "bound is %d ms)", MTX_GOROS, MTX_ITERS, dt, BLOCK_MS,
             BOUND_MS);
    check("ui_Run returns without waiting out an idle block",
          g_val == MTX_GOROS * MTX_ITERS && dt < BOUND_MS, detail);

    ui_MutexFree(g_mtx);
    ui_Fini();
}

/* ── 2. A blocked read must be woken by the completion ──
 *
 * Measured from the writer's side.  Without the ring's eventfd
 * registration nothing signals an I/O completion, so the reader would
 * only notice at the next block expiry — ~half of BLOCK_MS on average
 * and up to the full BLOCK_MS. */

#define LAT_ROUNDS 40

static int  g_pipe[2];
static double g_sent[LAT_ROUNDS];
static double g_seen[LAT_ROUNDS];
static volatile int g_got;

static void *
lat_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < LAT_ROUNDS; i++)
    {
        struct timespec sl = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&sl, NULL);
        char c = 'x';
        g_sent[i] = now_s();
        if (write(g_pipe[1], &c, 1) != 1) break;
        while (g_got <= i) {
            struct timespec s2 = { .tv_sec = 0, .tv_nsec = 200 * 1000 };
            nanosleep(&s2, NULL);
        }
    }
    return NULL;
}

static void
lat_reader(void)
{
    for (int i = 0; i < LAT_ROUNDS; i++)
    {
        char c;
        if (ui_Read(g_pipe[0], &c, 1) != 1) break;
        g_seen[i] = now_s();
        g_got = i + 1;
    }
}

static void
test_io_wakeup_latency(void)
{
    setenv("UI_IDLE_WAIT_MS", "2000", 1);
    if (ui_Init() != 0) exit(1);
    if (pipe(g_pipe) != 0) exit(1);

    pthread_t th;
    pthread_create(&th, NULL, lat_writer, NULL);
    ui_Go(lat_reader);
    ui_Run();
    pthread_join(th, NULL);
    close(g_pipe[0]);
    close(g_pipe[1]);
    ui_Fini();

    double sum = 0, mx = 0;
    int n = 0;
    for (int i = 0; i < LAT_ROUNDS; i++)
        if (g_seen[i] > 0 && g_sent[i] > 0) {
            double d = (g_seen[i] - g_sent[i]) * 1000.0;
            sum += d;
            if (d > mx) mx = d;
            n++;
        }
    if (n < LAT_ROUNDS / 2) {
        check("blocked ui_Read wakes on completion", false,
              "only observed a subset of writes");
        return;
    }

    char detail[160];
    snprintf(detail, sizeof detail,
             "n=%d mean=%.3f ms max=%.3f ms (block timeout is %d ms, "
             "bound is %d ms)", n, sum / n, mx, BLOCK_MS, BOUND_MS);
    check("blocked ui_Read wakes on completion, not on block expiry",
          mx < BOUND_MS, detail);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI: idle-wait regression (eventfd wake + shutdown broadcast)\n\n");

    test_shutdown_is_prompt();
    test_io_wakeup_latency();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}