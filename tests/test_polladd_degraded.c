/* Regression tests for ui_PollAdd's degraded-path fallbacks.
 *
 * ui_PollAdd used to fall back to poll(fd, 1, -1) whenever it could not
 * use the io_uring ring.  That is the only POSIX fallback in ui_io.c
 * that waits forever, and it wedged the whole vCPU thread inside
 * poll() with no way for the scheduler to preempt it -- ui_Run's
 * pthread_join never returned, so the process hung instead of failing.
 * The caller's O_NONBLOCK does not help: poll()'s blocking is governed
 * by its own timeout argument, not the fd's flags.
 *
 * Reachable in production because io_uring_setup charges the ring
 * against RLIMIT_MEMLOCK, a per-uid quota shared machine-wide with
 * delayed release, so it fails intermittently with ENOMEM under load.
 *
 * Two properties are pinned here:
 *   1. a ringless vCPU returns promptly instead of blocking forever;
 *   2. a ready fd still reports ready on that same ringless path, so
 *      callers retrying on EAGAIN make forward progress.
 *
 * UI_TEST_NO_RING=1 forces ui_vcpu_ensure_ring to fail, which is the
 * degraded state under test; no RLIMIT_MEMLOCK exhaustion required.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>

/* Generous enough to absorb a loaded box, far below the Makefile's
 * TEST_TIMEOUT so a regression fails the test instead of the suite. */
#define BOUND_MS 2000

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

/* ── 1. A not-ready fd must return promptly, not block forever ──
 *
 * A pipe read end with no writer-side data is never readable, so the
 * pre-fix poll(-1) parked the vCPU inside poll() until the Makefile
 * timeout killed the whole run.  Post-fix this must return < 0 with
 * EAGAIN, and the goro must stay schedulable. */
static atomic_int g_notready_ret;
static atomic_int g_notready_errno;

static void
probe_notready(void)
{
    /* A pipe read end with no data written is never readable. */
    int pfd[2];
    if (pipe(pfd) < 0) { atomic_store(&g_notready_ret, -1000); return; }

    errno = 0;
    int r = ui_PollAdd(pfd[0], 0x1 /* POLLIN */);

    atomic_store(&g_notready_ret, r);
    atomic_store(&g_notready_errno, errno);
    close(pfd[0]);
    close(pfd[1]);
}

/* ── 2. A ready fd must report ready even with no ring ──
 *
 * Otherwise callers that loop on EAGAIN (sana/src/net/conn.c does
 * exactly this) would spin without ever making progress. */
static atomic_int g_ready_ret;

static void
probe_ready(void)
{
    int pfd[2];
    if (pipe(pfd) < 0) { atomic_store(&g_ready_ret, -1000); return; }
    if (write(pfd[1], "x", 1) != 1) {
        atomic_store(&g_ready_ret, -1000);
        close(pfd[0]); close(pfd[1]);
        return;
    }

    int r = ui_PollAdd(pfd[0], 0x1 /* POLLIN */);
    atomic_store(&g_ready_ret, r);
    close(pfd[0]);
    close(pfd[1]);
}

static void
feeder(void)
{
    probe_notready();
    probe_ready();
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI: ui_PollAdd degraded-path fallback\n\n");

    setenv("UI_TEST_NO_RING", "1", 1);
    setenv("UI_NVCPUS", "2", 1);

    double t0 = now_s();
    if (ui_Init() != 0) {
        printf("  TEST: ui_Init ... FAIL\n");
        return 1;
    }
    ui_Go(feeder);
    ui_Run();
    double total_ms = (now_s() - t0) * 1000.0;
    ui_Fini();
    unsetenv("UI_TEST_NO_RING");

    int r1 = atomic_load(&g_notready_ret);
    char d1[160];
    snprintf(d1, sizeof d1,
             "ret=%d errno=%d (want ret<0 errno=EAGAIN=%d), run took %.0f ms",
             r1, atomic_load(&g_notready_errno), EAGAIN, total_ms);
    check("ringless ui_PollAdd returns instead of blocking forever",
          r1 < 0 && atomic_load(&g_notready_errno) == EAGAIN &&
          total_ms < BOUND_MS,
          d1);

    int r2 = atomic_load(&g_ready_ret);
    char d2[128];
    snprintf(d2, sizeof d2, "ret=%d (want 0)", r2);
    check("ringless ui_PollAdd still reports a ready fd", r2 == 0, d2);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}