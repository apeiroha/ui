#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

#define REPORT(op, us, count) do { \
    double ns = (double)(us) * 1000.0 / (count); \
    printf("%8.0f ns/op  (%d ops in %lluus)\n", ns, (int)(count), (unsigned long long)(us)); \
} while(0)

/* ── Goroutine spawn+exit throughput ── */

static int spawn_count = 0;

static void spawn_worker(void) {
    __sync_fetch_and_add(&spawn_count, 1);
}

static void bench_spawn(void) {
    int N = 10000;
    ui_Init();
    spawn_count = 0;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++)
        ui_Go(spawn_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_Fini();
    printf("  %-40s ", "goroutine spawn+exit");
    REPORT("spawn", end - start, N);
}

/* ── Chan send/recv thruput (buffered, same goro) ── */

static void bench_chan_same(void) {
    int N = 100000;
    ui_Init();
    uint64_t ch = ui_NewChan(sizeof(int), 1024);
    int v = 42;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++) {
        ui_ChanSend(ch, &v);
        ui_ChanRecv(ch, &v);
    }
    uint64_t end = now_us();
    ui_ChanFree(ch);
    ui_Fini();
    printf("  %-40s ", "chan send+recv (buffered)");
    REPORT("chan", end - start, N);
}

/* ── Chan try_send/try_recv thruput ── */

static void bench_chan_try(void) {
    int N = 100000;
    ui_Init();
    uint64_t ch = ui_NewChan(sizeof(int), 64);
    int v = 42;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++) {
        while (!ui_ChanTrySend(ch, &v));
        while (!ui_ChanTryRecv(ch, &v));
    }
    uint64_t end = now_us();
    ui_ChanFree(ch);
    ui_Fini();
    printf("  %-40s ", "chan try_send+recv");
    REPORT("chan_try", end - start, N);
}

/* ── Mutex: uncontested lock+unlock ── */

static void bench_mutex_uncontested(void) {
    int N = 2000000;
    ui_Init();
    uint64_t m = ui_MutexNew();
    uint64_t start = now_us();
    for (int i = 0; i < N; i++) {
        ui_MutexLock(m);
        ui_MutexUnlock(m);
    }
    uint64_t end = now_us();
    ui_MutexFree(m);
    ui_Fini();
    printf("  %-40s ", "mutex lock+unlock (uncontested)");
    REPORT("mutex_uncontested", end - start, N);
}

/* ── Mutex: uncontested TryLock ── */

static void bench_mutex_trylock(void) {
    int N = 2000000;
    ui_Init();
    uint64_t m = ui_MutexNew();
    uint64_t start = now_us();
    for (int i = 0; i < N; i++) {
        while (!ui_MutexTryLock(m));
        ui_MutexUnlock(m);
    }
    uint64_t end = now_us();
    ui_MutexFree(m);
    ui_Fini();
    printf("  %-40s ", "mutex trylock+unlock (uncontested)");
    REPORT("mutex_trylock", end - start, N);
}

/* ── Mutex: contended spin regime (empty critical section) ── */

static uint64_t cont_mtx_spin;
static int cont_val_spin = 0;

static void cont_spin_worker(void) {
    for (int i = 0; i < 1000; i++) {
        ui_MutexLock(cont_mtx_spin);
        cont_val_spin++;
        ui_MutexUnlock(cont_mtx_spin);
    }
}

static void bench_mutex_contended_spin(void) {
    int N = 50;
    ui_Init();
    cont_mtx_spin = ui_MutexNew();
    cont_val_spin = 0;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++)
        ui_Go(cont_spin_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_MutexFree(cont_mtx_spin);
    ui_Fini();
    printf("  %-40s ", "mutex contention spin (50 goros × 1000)");
    REPORT("cont_spin", end - start, N * 1000);
}

/* ── Mutex: contended work regime (park in kernel) ── */

static uint64_t cont_mtx_work;
static int cont_val_work = 0;

static void cont_work_worker(void) {
    for (int i = 0; i < 1000; i++) {
        ui_MutexLock(cont_mtx_work);
        for (int j = 0; j < 500; j++) {
            cont_val_work ^= cont_val_work * 1103515245 + 12345;
        }
        ui_MutexUnlock(cont_mtx_work);
    }
}

static void bench_mutex_contended_work(void) {
    int N = 50;
    ui_Init();
    cont_mtx_work = ui_MutexNew();
    cont_val_work = 0;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++)
        ui_Go(cont_work_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_MutexFree(cont_mtx_work);
    ui_Fini();
    printf("  %-40s ", "mutex contention work (50 goros × 1000)");
    REPORT("cont_work", end - start, N * 1000);
}

/* ── Mutex: legacy contended (100 iter, for comparison) ── */

static uint64_t cont_mtx;
static int cont_val = 0;

static void cont_worker(void) {
    for (int i = 0; i < 100; i++) {
        ui_MutexLock(cont_mtx);
        cont_val++;
        ui_MutexUnlock(cont_mtx);
    }
}

static void bench_mutex_contended(void) {
    int N = 50;
    ui_Init();
    cont_mtx = ui_MutexNew();
    cont_val = 0;
    uint64_t start = now_us();
    for (int i = 0; i < N; i++)
        ui_Go(cont_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_MutexFree(cont_mtx);
    ui_Fini();
    printf("  %-40s ", "mutex contention (50 goros × 100 iter)");
    REPORT("cont", end - start, N * 100);
}

/* ── Channel ping-pong (2 goroutines, unbuffered) ── */

static uint64_t pp_send_ch, pp_recv_ch;

static void pp_send_worker(void) {
    int v;
    for (int i = 0; i < 5000; i++) {
        v = i;
        ui_ChanSend(pp_send_ch, &v);
        ui_ChanRecv(pp_recv_ch, &v);
    }
}

static void pp_recv_worker(void) {
    int v;
    for (int i = 0; i < 5000; i++) {
        ui_ChanRecv(pp_send_ch, &v);
        v++;
        ui_ChanSend(pp_recv_ch, &v);
    }
}

static void bench_chan_pingpong(void) {
    ui_Init();
    pp_send_ch = ui_NewChan(sizeof(int), 0);
    pp_recv_ch = ui_NewChan(sizeof(int), 0);
    ui_Go(pp_send_worker);
    ui_Go(pp_recv_worker);
    uint64_t start = now_us();
    ui_Run();
    uint64_t end = now_us();
    ui_ChanFree(pp_send_ch); ui_ChanFree(pp_recv_ch);
    ui_Fini();
    printf("  %-40s ", "chan ping-pong (unbuffered, 2 goros)");
    REPORT("pp", end - start, 5000);
}

/* ── Local fanout spawn (exercises work stealing) ── */

static int fanout_count = 0;

static void fanout_worker(void) {
    __sync_fetch_and_add(&fanout_count, 1);
}

static void fanout_root(void) {
    for (int i = 0; i < 10000; i++)
        ui_Go(fanout_worker);
}

static void bench_local_fanout(void) {
    int N = 10000;
    ui_Init();
    fanout_count = 0;
    uint64_t start = now_us();
    ui_Go(fanout_root);
    ui_Run();
    uint64_t end = now_us();
    ui_Fini();
    printf("  %-40s ", "local fanout spawn+exit");
    REPORT("fanout", end - start, N);
}

/* ── RwLock: read-dense scenarios ── */

static uint64_t rwlock_bench;
static int rwlock_iter = 0;

static void rwlock_read_worker(void) {
    for (int i = 0; i < rwlock_iter; i++) {
        ui_RwLockRLock(rwlock_bench);
        ui_RwLockRUnlock(rwlock_bench);
    }
}

static void rwlock_write_worker(void) {
    for (int i = 0; i < rwlock_iter; i++) {
        ui_RwLockWLock(rwlock_bench);
        ui_RwLockWUnlock(rwlock_bench);
    }
}

/* Mixed: 4 readers + 1 writer with a small write-side hold window */
static void rwlock_mixed_writer(void) {
    for (int i = 0; i < 2000; i++) {
        ui_RwLockWLock(rwlock_bench);
        volatile int x = 0;
        for (int j = 0; j < 500; j++) x += j;   /* hold window */
        (void)x;
        ui_RwLockWUnlock(rwlock_bench);
        ui_Yield();
    }
}

static void bench_rwlock_read_single(void) {
    int N = 1000000;
    rwlock_iter = N;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    uint64_t start = now_us();
    ui_Go(rwlock_read_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
    printf("  %-40s ", "rwlock read (single goro)");
    REPORT("rwl_read1", end - start, N);
}

static void bench_rwlock_read_multi(void) {
    int N = 500000;
    int NG = 4;
    rwlock_iter = N;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    uint64_t start = now_us();
    for (int i = 0; i < NG; i++)
        ui_Go(rwlock_read_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
    printf("  %-40s ", "rwlock read (4 goros, multi-vcpu)");
    REPORT("rwl_read4", end - start, (uint64_t)N * NG);
}

static void bench_rwlock_rw_mixed(void) {
    int N = 500000;
    int NG = 4;
    rwlock_iter = N;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    uint64_t start = now_us();
    for (int i = 0; i < NG; i++)
        ui_Go(rwlock_read_worker);
    ui_Go(rwlock_mixed_writer);
    ui_Run();
    uint64_t end = now_us();
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
    printf("  %-40s ", "rwlock read (4 goros + writer)");
    REPORT("rwl_rw", end - start, (uint64_t)N * NG);
}

static void bench_rwlock_write_multi(void) {
    int N = 200000;
    int NG = 4;
    rwlock_iter = N;
    ui_Init();
    rwlock_bench = ui_RwLockNew();
    uint64_t start = now_us();
    for (int i = 0; i < NG; i++)
        ui_Go(rwlock_write_worker);
    ui_Run();
    uint64_t end = now_us();
    ui_RwLockFree(rwlock_bench);
    ui_Fini();
    printf("  %-40s ", "rwlock write (4 goros, multi-vcpu)");
    REPORT("rwl_write4", end - start, (uint64_t)N * NG);
}

/* ── Main ── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI (うい) Benchmarks ===\n");
    printf("  Platform: x86_64-linux-musl (clang, -O3, static)\n\n");

    bench_spawn();
    bench_chan_same();
    bench_chan_try();
    bench_mutex_uncontested();
    bench_mutex_trylock();
    bench_mutex_contended_spin();
    bench_mutex_contended_work();
    bench_mutex_contended();
    bench_chan_pingpong();
    bench_local_fanout();
    bench_rwlock_read_single();
    bench_rwlock_read_multi();
    bench_rwlock_rw_mixed();
    bench_rwlock_write_multi();

    printf("\n=== Done ===\n");
    return 0;
}
