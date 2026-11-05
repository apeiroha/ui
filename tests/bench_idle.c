/* Idle-path benchmarks.
 *
 * A parked vCPU is woken by the event itself (cross-vCPU wakeup, I/O
 * completion, sleepq deadline); UI_IDLE_WAIT_MS only covers a wakeup
 * that was somehow lost.  That claim is what makes a large backstop
 * safe, and it is not observable from the other benchmark groups - they
 * never sit idle.  Two things are measured here:
 *
 *   - wake latency: how long a parked vCPU takes to start running a
 *     goroutine whose I/O just completed, with a deliberately long
 *     backstop installed.  If a regression made the backstop the real
 *     wake source this goes from microseconds to ~idle_wait_ms, a
 *     signal too large to miss.
 *
 *   - idle CPU: what the backstop costs when nothing happens.  Each
 *     parked vCPU pays one syscall per expiry, so the cost is
 *     proportional to the number of vCPUs and inversely proportional to
 *     the backstop.  The syscall probe reads the same benchmark and
 *     reports the wakeup rate directly (ppoll per millisecond), which
 *     is exact where the CPU figure is coarse.
 */
#include "ui.h"
#include "bench.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double
now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Process CPU (user + system), microseconds.  clock_gettime keeps its
 * own accounting and matches getrusage here to within a jiffy. */
static double
proc_cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* The knob each benchmark runs under, passed through the harness's
 * opaque argument (a global would be overwritten by the next
 * registration - every benchmark would then run the last value). */
typedef struct { int wait_ms; } idle_cfg;

/* ── Wake latency ──
 *
 * A peer thread writes into a pipe after a sleep; a goroutine is parked
 * in ui_Read on it.  The writer timestamps before the write, the reader
 * timestamps when it returns, and the difference is the wake latency:
 * the work the runtime does between "the completion landed" and "the
 * goroutine is running again".
 */

#define LAT_ROUNDS   50
#define LAT_SLEEP_US (2 * 1000)      /* park long enough to be idle */
#define LAT_ACK_POLL_US 200

static int    lat_pipe[2];
static double lat_sent[LAT_ROUNDS];
static double lat_seen[LAT_ROUNDS];
static volatile int lat_ack;
static int    lat_rounds;

static void *
lat_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < lat_rounds; i++)
    {
        struct timespec sl = { .tv_sec = 0, .tv_nsec = LAT_SLEEP_US * 1000 };
        nanosleep(&sl, NULL);
        char c = 'x';
        lat_sent[i] = now_s();
        if (write(lat_pipe[1], &c, 1) != 1)
            break;
        /* Wait for the reader to record the round before the next one,
         * so the two arrays stay in step. */
        while (lat_ack <= i)
        {
            struct timespec s2 = { .tv_sec = 0, .tv_nsec = LAT_ACK_POLL_US * 1000 };
            nanosleep(&s2, NULL);
        }
    }
    return NULL;
}

static void
lat_reader(void)
{
    for (int i = 0; i < lat_rounds; i++)
    {
        char c;
        if (ui_Read(lat_pipe[0], &c, 1) != 1)
            break;
        lat_seen[i] = now_s();
        lat_ack = i + 1;
    }
}

static void
bench_idle_wake(ui_bench_ctx *b, int n)
{
    int wait_ms = ((const idle_cfg *)b->arg)->wait_ms;
    int rounds = n > 0 ? n : LAT_ROUNDS;
    if (rounds > LAT_ROUNDS)
        rounds = LAT_ROUNDS;

    ui_SetIdleWaitMs((uint32_t)wait_ms);
    ui_Init();
    if (pipe(lat_pipe) != 0)
    {
        ui_bench_set_ops(b, 0);
        ui_Fini();
        return;
    }
    memset(lat_sent, 0, sizeof(lat_sent));
    memset(lat_seen, 0, sizeof(lat_seen));
    lat_ack = 0;
    lat_rounds = rounds;

    pthread_t th;
    if (pthread_create(&th, NULL, lat_writer, NULL) != 0)
    {
        ui_bench_set_ops(b, 0);
        ui_Fini();
        return;
    }
    ui_Go(lat_reader);
    ui_Run();
    pthread_join(th, NULL);

    double sum = 0.0, mx = 0.0;
    int seen = 0;
    for (int i = 0; i < rounds; i++)
    {
        if (lat_seen[i] <= 0 || lat_sent[i] <= 0)
            continue;
        double us = (lat_seen[i] - lat_sent[i]) * 1e6;
        sum += us;
        if (us > mx)
            mx = us;
        seen++;
    }
    close(lat_pipe[0]);
    close(lat_pipe[1]);
    ui_Fini();

    ui_bench_set_ops(b, seen);
    if (seen == 0)
    {
        ui_bench_report(b, 0.0, "us");
        return;
    }
    ui_bench_report(b, sum / (double)seen, "us");
    ui_bench_report_extra(b, "max", mx, "us");
    ui_bench_report_extra(b, "idle_wait", (double)wait_ms, "ms");
}

/* ── Idle CPU ──
 *
 * One goroutine sleeps for the whole window; every vCPU has nothing to
 * run, so all of them sit in the idle wait.  The process CPU burned
 * over the window is the cost of the backstop expiries.
 *
 * The window has to outlast that cost: creating and joining the vCPU
 * threads is worth ~1.4ms on 4 vCPUs, which would otherwise be most of
 * what even a 1000ms backstop burns.  With a second-long window the
 * 10 / 100 / 1000ms backstops come out at roughly 26 / 3.4 / 1.4ms, a
 * clean ladder above the floor.
 *
 * The syscall probe reads the same benchmarks and divides by the window,
 * so the exact rate (ppoll per millisecond of idle - 0.38 / 0.04 / 0.004
 * for those three) is available too, where the CPU figure is coarse.
 *
 * One round each, no repeats: the measurement *is* the window, and
 * repeating a second-long window would cost more than it adds.  The
 * trade is that a busy machine inflates the figure (a compiler running
 * alongside once tripled it), so the ppoll rate is the number to trust
 * for a regression; the CPU figure is the one a reader recognises.
 */

#define IDLE_WINDOW_MS 1000

static int idle_window_ms = IDLE_WINDOW_MS;

static void
idle_sleeper(void)
{
    ui_Sleep((unsigned)idle_window_ms);
}

static void
bench_idle_cpu(ui_bench_ctx *b, int n)
{
    int wait_ms = ((const idle_cfg *)b->arg)->wait_ms;
    (void)n;                     /* the window is the measurement */
    ui_SetIdleWaitMs((uint32_t)wait_ms);
    ui_Init();
    ui_Go(idle_sleeper);

    double c0 = proc_cpu_ms();
    ui_Run();
    double c1 = proc_cpu_ms();
    int nvcpus = ui_NVCPUs();    /* read before Fini clears the scheduler */

    ui_Fini();
    ui_bench_set_ops(b, idle_window_ms);
    ui_bench_report(b, c1 - c0, "ms CPU");
    ui_bench_report_extra(b, "window", (double)idle_window_ms, "ms");
    ui_bench_report_extra(b, "idle_wait", (double)wait_ms, "ms");
    ui_bench_report_extra(b, "nvcpus", (double)nvcpus, "count");
}

/* ── Main ── */

static const idle_cfg wake_long   = { 2000 };   /* backstop far away */
static const idle_cfg wake_short  = { 5 };      /* backstop very close */
static const idle_cfg cpu_10      = { 10 };
static const idle_cfg cpu_100     = { 100 };    /* the default */
static const idle_cfg cpu_1000    = { 1000 };

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI idle-path benchmarks ===\n\n");

    /* Same body twice: the latency must not depend on the backstop. */
    ui_bench_add("bench_idle", "idle wake latency (UI_IDLE_WAIT_MS=2000)",
                 bench_idle_wake, (void *)&wake_long, LAT_ROUNDS);
    ui_bench_add("bench_idle", "idle wake latency (UI_IDLE_WAIT_MS=5)",
                 bench_idle_wake, (void *)&wake_short, LAT_ROUNDS);

    /* The backstop's cost: one syscall per expiry per parked vCPU.  A
     * single round each - the measurement *is* the window. */
    ui_bench_add_ex("bench_idle", "idle CPU (UI_IDLE_WAIT_MS=10)",
                    bench_idle_cpu, (void *)&cpu_10, UI_BENCH_ONCE, 0);
    ui_bench_add_ex("bench_idle", "idle CPU (UI_IDLE_WAIT_MS=100)",
                    bench_idle_cpu, (void *)&cpu_100, UI_BENCH_ONCE, 0);
    ui_bench_add_ex("bench_idle", "idle CPU (UI_IDLE_WAIT_MS=1000)",
                    bench_idle_cpu, (void *)&cpu_1000, UI_BENCH_ONCE, 0);

    int rc = ui_bench_main(argc, argv);
    printf("\n=== Done ===\n");
    return rc;
}
