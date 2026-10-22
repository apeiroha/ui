/* UI (うい) scheduler and synchronisation benchmarks.
 *
 * Modeled on Go's testing.B: the harness picks the iteration count,
 * warms up, repeats the measurement and reports a distribution, so the
 * numbers can be compared with a confidence interval instead of by
 * eyeballing a single ns/op.
 *
 * The metric names are the ones the dashboard already tracks - do not
 * rename them without also updating web/src/metrics.test.ts.
 */
#include "ui.h"
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Goroutine spawn+exit (batch) ──
 *
 * Spawns n goroutines back to back and then runs them.  The batch is
 * far larger than the pre-allocated pool, so most goroutines need a
 * fresh stack: this is the "cold" end of the spawn cost.
 */

static int spawn_count;

static void
spawn_worker(void)
{
    __sync_fetch_and_add(&spawn_count, 1);
}

static void
bench_spawn(ui_bench_ctx *b, int n)
{
    ui_Init();
    spawn_count = 0;
    ui_bench_start(b);
    for (int i = 0; i < n; i++)
        ui_Go(spawn_worker);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_Fini();
}

/* ── Goroutine spawn+exit (steady state, pool hit) ──
 *
 * Spawn a wave of goroutines, wait for them to finish, repeat.  The
 * finished goroutines went back to the pool, so the next wave reuses
 * their stacks: this is the recycle path the stack trim policy
 * governs.  The wave size (256) and the completion signal (a counter
 * the root spins on with ui_Yield) match the steady-state
 * measurements taken during the optimisation work, so the numbers are
 * comparable.
 */

#define WARM_WAVE 256

static int      warm_done;
static int      warm_waves;

static void
warm_child(void)
{
    __sync_fetch_and_add(&warm_done, 1);
}

static void
warm_root(void)
{
    for (int w = 0; w < warm_waves; w++)
    {
        int target = __atomic_load_n(&warm_done, __ATOMIC_RELAXED) + WARM_WAVE;
        for (int i = 0; i < WARM_WAVE; i++)
            ui_Go(warm_child);
        while (__atomic_load_n(&warm_done, __ATOMIC_RELAXED) < target)
            ui_Yield();
    }
}

static void
bench_spawn_warm(ui_bench_ctx *b, int n)
{
    int waves = n / WARM_WAVE;
    if (waves < 1)
        waves = 1;
    ui_Init();
    warm_done = 0;

    /* Reach steady state *before* the measurement window opens.  The
     * first wave pays a one-off price (fresh stacks: mmap, mprotect and
     * first-touch faults, ~25ms for 256 goroutines) that has nothing to
     * do with the steady-state recycle cost.  Leaving it inside the
     * window would make the reported per-op cost depend on the batch
     * size - the same workload would report 3.9us/op at 12800
     * goroutines and 2.1us/op at 200000, purely because of how the
     * one-off cost was amortised. */
    warm_waves = 1;
    ui_Go(warm_root);
    ui_Run();

    warm_waves = waves;
    ui_bench_start(b);
    ui_Go(warm_root);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, waves * WARM_WAVE);
    ui_Fini();
}

/* ── Chan send/recv (buffered, same goroutine) ── */

static void
bench_chan_same(ui_bench_ctx *b, int n)
{
    ui_Init();
    uint64_t ch = ui_NewChan(sizeof(int), 1024);
    int v = 42;
    ui_bench_start(b);
    for (int i = 0; i < n; i++)
    {
        ui_ChanSend(ch, &v);
        ui_ChanRecv(ch, &v);
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_ChanFree(ch);
    ui_Fini();
}

/* ── Chan try_send/try_recv ── */

static void
bench_chan_try(ui_bench_ctx *b, int n)
{
    ui_Init();
    uint64_t ch = ui_NewChan(sizeof(int), 64);
    int v = 42;
    ui_bench_start(b);
    for (int i = 0; i < n; i++)
    {
        while (!ui_ChanTrySend(ch, &v));
        while (!ui_ChanTryRecv(ch, &v));
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_ChanFree(ch);
    ui_Fini();
}

/* ── Mutex: uncontended lock+unlock ── */

static void
bench_mutex_uncontested(ui_bench_ctx *b, int n)
{
    ui_Init();
    uint64_t m = ui_MutexNew();
    ui_bench_start(b);
    for (int i = 0; i < n; i++)
    {
        ui_MutexLock(m);
        ui_MutexUnlock(m);
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_MutexFree(m);
    ui_Fini();
}

/* ── Mutex: uncontended TryLock ── */

static void
bench_mutex_trylock(ui_bench_ctx *b, int n)
{
    ui_Init();
    uint64_t m = ui_MutexNew();
    ui_bench_start(b);
    for (int i = 0; i < n; i++)
    {
        while (!ui_MutexTryLock(m));
        ui_MutexUnlock(m);
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_MutexFree(m);
    ui_Fini();
}

/* ── Mutex: contended, empty critical section ── */

#define CONT_GOROS 50
#define CONT_ITERS 1000

static uint64_t cont_mtx_spin;
static int      cont_val_spin;

static void
cont_spin_worker(void)
{
    for (int i = 0; i < CONT_ITERS; i++)
    {
        ui_MutexLock(cont_mtx_spin);
        cont_val_spin++;
        ui_MutexUnlock(cont_mtx_spin);
    }
}

static void
bench_mutex_contended_spin(ui_bench_ctx *b, int n)
{
    /* n counts the lock/unlock pairs; the scenario runs in rounds of
     * CONT_GOROS goroutines so the contention is the same every time. */
    int rounds = n / (CONT_GOROS * CONT_ITERS);
    if (rounds < 1)
        rounds = 1;
    ui_Init();
    cont_mtx_spin = ui_MutexNew();
    cont_val_spin = 0;
    ui_bench_start(b);
    for (int r = 0; r < rounds; r++)
    {
        for (int i = 0; i < CONT_GOROS; i++)
            ui_Go(cont_spin_worker);
        ui_Run();
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, rounds * CONT_GOROS * CONT_ITERS);
    ui_MutexFree(cont_mtx_spin);
    ui_Fini();
}

/* ── Mutex: contended, the lock is held while working ── */

#define CONT_WORK_INNER 500

static uint64_t cont_mtx_work;
static int      cont_val_work;

static void
cont_work_worker(void)
{
    for (int i = 0; i < CONT_ITERS; i++)
    {
        ui_MutexLock(cont_mtx_work);
        for (int j = 0; j < CONT_WORK_INNER; j++)
            cont_val_work ^= cont_val_work * 1103515245 + 12345;
        ui_MutexUnlock(cont_mtx_work);
    }
}

static void
bench_mutex_contended_work(ui_bench_ctx *b, int n)
{
    int rounds = n / (CONT_GOROS * CONT_ITERS);
    if (rounds < 1)
        rounds = 1;
    ui_Init();
    cont_mtx_work = ui_MutexNew();
    cont_val_work = 0;
    ui_bench_start(b);
    for (int r = 0; r < rounds; r++)
    {
        for (int i = 0; i < CONT_GOROS; i++)
            ui_Go(cont_work_worker);
        ui_Run();
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, rounds * CONT_GOROS * CONT_ITERS);
    ui_MutexFree(cont_mtx_work);
    ui_Fini();
}

/* ── Mutex: legacy contended scenario (fewer iterations) ── */

#define CONT_LEGACY_ITERS 100

static uint64_t cont_mtx;
static int      cont_val;

static void
cont_worker(void)
{
    for (int i = 0; i < CONT_LEGACY_ITERS; i++)
    {
        ui_MutexLock(cont_mtx);
        cont_val++;
        ui_MutexUnlock(cont_mtx);
    }
}

static void
bench_mutex_contended(ui_bench_ctx *b, int n)
{
    int rounds = n / (CONT_GOROS * CONT_LEGACY_ITERS);
    if (rounds < 1)
        rounds = 1;
    ui_Init();
    cont_mtx = ui_MutexNew();
    cont_val = 0;
    ui_bench_start(b);
    for (int r = 0; r < rounds; r++)
    {
        for (int i = 0; i < CONT_GOROS; i++)
            ui_Go(cont_worker);
        ui_Run();
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, rounds * CONT_GOROS * CONT_LEGACY_ITERS);
    ui_MutexFree(cont_mtx);
    ui_Fini();
}

/* ── Channel ping-pong (two goroutines, unbuffered) ── */

#define PP_ROUND_TRIPS 5000

static uint64_t pp_send_ch, pp_recv_ch;
static int      pp_rounds;

static void
pp_send_worker(void)
{
    int v;
    for (int i = 0; i < PP_ROUND_TRIPS; i++)
    {
        v = i;
        ui_ChanSend(pp_send_ch, &v);
        ui_ChanRecv(pp_recv_ch, &v);
    }
}

static void
pp_recv_worker(void)
{
    int v;
    for (int i = 0; i < PP_ROUND_TRIPS; i++)
    {
        ui_ChanRecv(pp_send_ch, &v);
        v++;
        ui_ChanSend(pp_recv_ch, &v);
    }
}

static void
bench_chan_pingpong(ui_bench_ctx *b, int n)
{
    int rounds = n / PP_ROUND_TRIPS;
    if (rounds < 1)
        rounds = 1;
    pp_rounds = rounds;
    ui_Init();
    ui_bench_start(b);
    for (int r = 0; r < rounds; r++)
    {
        pp_send_ch = ui_NewChan(sizeof(int), 0);
        pp_recv_ch = ui_NewChan(sizeof(int), 0);
        ui_Go(pp_send_worker);
        ui_Go(pp_recv_worker);
        ui_Run();
        ui_ChanFree(pp_send_ch);
        ui_ChanFree(pp_recv_ch);
    }
    ui_bench_stop(b);
    ui_bench_set_ops(b, rounds * PP_ROUND_TRIPS);
    ui_Fini();
    (void)pp_rounds;
}

/* ── Local fanout spawn (exercises work stealing) ── */

static int      fanout_count;
static int      fanout_n;

static void
fanout_worker(void)
{
    __sync_fetch_and_add(&fanout_count, 1);
}

static void
fanout_root(void)
{
    for (int i = 0; i < fanout_n; i++)
        ui_Go(fanout_worker);
}

static void
bench_local_fanout(ui_bench_ctx *b, int n)
{
    ui_Init();
    fanout_count = 0;
    fanout_n = n;
    ui_bench_start(b);
    ui_Go(fanout_root);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_Fini();
}

/* ── RwLock: read-dense scenarios ── */

static uint64_t rwlock_bench;
static int      rwlock_iter;

static void
rwlock_read_worker(void)
{
    for (int i = 0; i < rwlock_iter; i++)
    {
        ui_RwLockRLock(rwlock_bench);
        ui_RwLockRUnlock(rwlock_bench);
    }
}

static void
rwlock_write_worker(void)
{
    for (int i = 0; i < rwlock_iter; i++)
    {
        ui_RwLockWLock(rwlock_bench);
        ui_RwLockWUnlock(rwlock_bench);
    }
}

/* Mixed: four readers plus one writer with a small write-side hold */
static void
rwlock_mixed_writer(void)
{
    for (int i = 0; i < 2000; i++)
    {
        ui_RwLockWLock(rwlock_bench);
        volatile int x = 0;
        for (int j = 0; j < 500; j++) x += j;
        (void)x;
        ui_RwLockWUnlock(rwlock_bench);
        ui_Yield();
    }
}

static void
bench_rwlock_read_single(ui_bench_ctx *b, int n)
{
    rwlock_iter = n;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    ui_bench_start(b);
    ui_Go(rwlock_read_worker);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, n);
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
}

static void
bench_rwlock_read_multi(ui_bench_ctx *b, int n)
{
    int ng = 4;
    int per = n / ng;
    if (per < 1)
        per = 1;
    rwlock_iter = per;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    ui_bench_start(b);
    for (int i = 0; i < ng; i++)
        ui_Go(rwlock_read_worker);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, per * ng);
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
}

static void
bench_rwlock_rw_mixed(ui_bench_ctx *b, int n)
{
    int ng = 4;
    int per = n / ng;
    if (per < 1)
        per = 1;
    rwlock_iter = per;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    ui_bench_start(b);
    for (int i = 0; i < ng; i++)
        ui_Go(rwlock_read_worker);
    ui_Go(rwlock_mixed_writer);
    ui_Run();
    ui_bench_stop(b);
    /* The writer's own lock/unlock pairs are not counted: the metric
     * describes the read path. */
    ui_bench_set_ops(b, per * ng);
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
}

static void
bench_rwlock_write_multi(ui_bench_ctx *b, int n)
{
    int ng = 4;
    int per = n / ng;
    if (per < 1)
        per = 1;
    rwlock_iter = per;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    ui_bench_start(b);
    for (int i = 0; i < ng; i++)
        ui_Go(rwlock_write_worker);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, per * ng);
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
}

/* ── Main ── */

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI (うい) Benchmarks ===\n");
    printf("  Platform: x86_64-linux-musl (clang, -O3, static)\n\n");

    /* The op counts are part of the metric definitions: they are the
     * ones the dashboard has been tracking, so a change shows up as a
     * jump in the series rather than as a silent redefinition. */
    ui_bench_add("bench_ui", "goroutine spawn+exit", bench_spawn, NULL, 10000);
    ui_bench_add("bench_ui", "goroutine spawn+exit (warm, pool hit)",
                 bench_spawn_warm, NULL, 100000);
    ui_bench_add("bench_ui", "chan send+recv (buffered)", bench_chan_same, NULL,
                 100000);
    ui_bench_add("bench_ui", "chan try_send+recv", bench_chan_try, NULL, 100000);
    ui_bench_add("bench_ui", "mutex lock+unlock (uncontested)",
                 bench_mutex_uncontested, NULL, 2000000);
    ui_bench_add("bench_ui", "mutex trylock+unlock (uncontested)",
                 bench_mutex_trylock, NULL, 2000000);
    ui_bench_add("bench_ui", "mutex contention spin (50 goros × 1000)",
                 bench_mutex_contended_spin, NULL, 500000);
    ui_bench_add("bench_ui", "mutex contention work (50 goros × 1000)",
                 bench_mutex_contended_work, NULL, 50000);
    ui_bench_add("bench_ui", "mutex contention (50 goros × 100 iter)",
                 bench_mutex_contended, NULL, 50000);
    ui_bench_add("bench_ui", "chan ping-pong (unbuffered, 2 goros)",
                 bench_chan_pingpong, NULL, 50000);
    ui_bench_add("bench_ui", "local fanout spawn+exit", bench_local_fanout, NULL,
                 10000);
    ui_bench_add("bench_ui", "rwlock read (single goro)",
                 bench_rwlock_read_single, NULL, 1000000);
    ui_bench_add("bench_ui", "rwlock read (4 goros, multi-vcpu)",
                 bench_rwlock_read_multi, NULL, 2000000);
    ui_bench_add("bench_ui", "rwlock read (4 goros + writer)",
                 bench_rwlock_rw_mixed, NULL, 2000000);
    ui_bench_add("bench_ui", "rwlock write (4 goros, multi-vcpu)",
                 bench_rwlock_write_multi, NULL, 800000);

    int rc = ui_bench_main(argc, argv);
    printf("\n=== Done ===\n");
    return rc;
}
