#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* P0 confirmation tests — each test encodes the EXPECTED (fixed) behavior.
 * They FAIL on the current library code, confirming the bug exists.
 *   T1: ChanSend on a closed channel is silently dropped (should be fatal)
 *   T2: TimerStop does not cancel the timer goroutine
 *   T3: RecvMultiClose frees rm before the kernel confirms the cancel
 *       (use-after-free — observable under ASan; see test-p0-asan target)
 *   T4: ui.h declares functions after the include-guard #endif
 */

static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) do { \
    printf("  TEST: %s ... ", name); \
    fflush(stdout); \
} while (0)

#define PASS() do { printf("PASS\n"); test_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); test_failed++; } while (0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)

/* ── T1: send on closed channel ──
 * Go semantics: send on a closed channel panics.  The library currently
 * returns silently and the value is dropped.  A panic would abort the
 * child process before it writes the "NO_PANIC" marker. */
static void
test_chan_send_closed(void)
{
    TEST("T1 ChanSend on closed channel must panic, not silently drop");
    int fds[2];
    if (pipe(fds) < 0) { FAIL("pipe failed"); return; }
    pid_t pid = fork();
    if (pid < 0) { FAIL("fork failed"); return; }
    if (pid == 0)
    {
        close(fds[0]);
        if (ui_Init() != 0) _exit(2);
        uint64_t ch = ui_NewChan(sizeof(uint64_t), 4);
        if (!ch) _exit(3);
        ui_ChanClose(ch);
        uint64_t v = 0xDEADBEEF;
        ui_ChanSend(ch, &v);
        /* Reached only if send-on-closed returned silently */
        write(fds[1], "NO_PANIC", 9);
        _exit(0);
    }
    close(fds[1]);
    char buf[16] = {0};
    read(fds[0], buf, sizeof(buf) - 1);
    int status = 0;
    waitpid(pid, &status, 0);
    close(fds[0]);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        FAIL("send to closed channel returned silently (value dropped, no error)");
    else
        PASS();
}

/* ── T2: TimerStop must cancel the timer goroutine ──
 * Expected: after TimerStop, the timer goro is gone (active_count back to
 * baseline).  Current code: TimerStop only closes the channel; the timer
 * goro lives until its deadline, then sends into the closed channel. */
static int t2_base, t2_after_create, t2_after_stop, t2_after_deadline;

static void
timer_worker(void)
{
    t2_base = atomic_load(&g_ui_sched.active_count);
    uint64_t ch = ui_NewTimer(150);
    t2_after_create = atomic_load(&g_ui_sched.active_count);
    ui_TimerStop(ch);
    ui_Sleep(10);   /* give the woken timer goro a chance to exit */
    t2_after_stop = atomic_load(&g_ui_sched.active_count);
    ui_Sleep(400);   /* wait past the 150ms deadline */
    t2_after_deadline = atomic_load(&g_ui_sched.active_count);
}

static void
test_timer_stop(void)
{
    TEST("T2 TimerStop must cancel the timer goroutine");
    setenv("UI_NVCPUS", "1", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    ui_Go(timer_worker);
    ui_Run();
    ui_Fini();
    ASSERT(t2_after_create == t2_base + 1, "active_count after NewTimer");
    if (t2_after_stop != t2_base)
    {
        printf("FAIL: timer goroutine alive after TimerStop "
               "(active=%d, base=%d)\n", t2_after_stop, t2_base);
        test_failed++;
        return;
    }
    if (t2_after_deadline != t2_base)
    {
        FAIL("timer goroutine did not exit even after deadline");
        return;
    }
    PASS();
}

/* ── T3: RecvMultiClose use-after-free ──
 * Close submits an async cancel then frees rm.  A packet that lands in the
 * kernel after close's GETEVENTS but before the cancel takes effect yields
 * a data CQE (user_data = rm|1) that is only read at a LATER drain, when
 * rm is already freed → ui_uring_drain reads rm->active on freed memory.
 * This is a race, so the plain build usually "passes"; the test-p0-asan
 * target (ASan) is the actual detector. */
static atomic_int t3_cb_count;
static atomic_int t3_churn_done;
static int t3_sv[2];

static void
t3_recv_cb(void *ctx, struct sockaddr *from, socklen_t from_len,
           const void *data, size_t len)
{
    (void)from; (void)from_len; (void)data; (void)len;
    atomic_fetch_add((atomic_int *)ctx, 1);
}

/* Concurrent traffic: packets land at random moments relative to Close,
 * some inside the [close's GETEVENTS, cancel-effective] window.
 * Must never block: a blocking write() on a full socket buffer would stall
 * the whole cooperative vCPU. */
static void
t3_traffic(void)
{
    int flags = fcntl(t3_sv[1], F_GETFL, 0);
    fcntl(t3_sv[1], F_SETFL, flags | O_NONBLOCK);
    while (!atomic_load(&t3_churn_done))
    {
        char pkt = 'x';
        ssize_t n = write(t3_sv[1], &pkt, 1);
        if (n < 0 && errno == EAGAIN)
            ui_SleepUs(200);   /* buffer full: wait for churn to drain */
        else
            ui_SleepUs(20);
    }
}

static void
t3_churn(void)
{
    for (int i = 0; i < 3000; i++)
    {
        struct ui_RecvMulti *rm = ui_RecvMulti(t3_sv[0], t3_recv_cb, &t3_cb_count);
        if (!rm) continue;
        ui_SleepUs(30);
        ui_RecvMultiClose(rm);
        ui_SleepUs(30);
    }
    atomic_store(&t3_churn_done, 1);
}

static void
test_recvmulti_close(void)
{
    TEST("T3 RecvMultiClose frees rm before cancel CQE (ASan-detectable)");
    setenv("UI_NVCPUS", "1", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    atomic_store(&t3_cb_count, 0);
    atomic_store(&t3_churn_done, 0);
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, t3_sv) < 0)
    {
        FAIL("socketpair failed");
        ui_Fini();
        return;
    }
    ui_Go(t3_traffic);
    ui_Go(t3_churn);
    ui_Run();
    close(t3_sv[0]);
    close(t3_sv[1]);
    ui_Fini();
    /* The race is not deterministic in a plain build; ASan run reports it. */
    PASS();
}

/* ── T4: ui.h must not declare anything after the include-guard #endif ── */
static void
test_ui_h_guard(void)
{
    TEST("T4 ui.h has no declarations after #endif");
    FILE *f = fopen("ui.h", "r");
    if (!f) { FAIL("cannot open ui/ui.h"); return; }
    char line[512];
    int lineno = 0, last_endif = -1, bad = -1;
    while (fgets(line, sizeof(line), f))
    {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "#endif", 6) == 0) last_endif = lineno;
    }
    rewind(f);
    lineno = 0;
    while (fgets(line, sizeof(line), f))
    {
        lineno++;
        if (lineno <= last_endif) continue;
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == 0) continue;                       /* blank */
        if (strncmp(p, "/*", 2) == 0) continue;      /* comment */
        if (*p == '*') continue;                     /* comment continuation */
        if (strncmp(p, "//", 2) == 0) continue;      /* comment */
        bad = lineno;
        break;
    }
    fclose(f);
    ASSERT(last_endif > 0, "no #endif found in ui/ui.h");
    if (bad > 0)
    {
        printf("FAIL: code after #endif at line %d\n", bad);
        test_failed++;
        return;
    }
    PASS();
}

int
main(void)
{
    printf("=== P0 confirmation tests (expect failures on current code) ===\n");
    test_chan_send_closed();
    test_timer_stop();
    test_recvmulti_close();
    test_ui_h_guard();
    printf("\n=== Results: %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
