/* Regression test: a lone spawned goro must be stealable.
 *
 * ui_steal_work previously required victim runq count > 2, so a single
 * `go feeder()` enqueued on the spawner's own vCPU never migrated: the
 * spawner holds its vCPU until it blocks (no preemption) and idle peers
 * declined to steal a runq of depth 1 — the spawned goro starved until
 * the spawner yielded.  Observed as "go feeder() ran synchronously" in
 * HTTP bench chunked-response workloads (large chunk fills the chan
 * buffer on the first write, handler parks immediately after spawning).
 *
 * Scenarios (skipped under nvcpus == 1 — sequential by contract):
 *   B: lone feeder + busy non-blocking spawner -> feeder must progress
 *      within DEADLINE_MS once any peer vCPU is idle
 *   A: spawn + immediate park -> spawner must resume via handoff
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#define SPIN_MS 100

static atomic_int prog;
static atomic_int done_a;
static uint64_t chan_a;

static volatile unsigned sink;

static void spin_ms(unsigned ms)
{
    /* ~200k iters/ms at O1 on this class of machine; only needs to be
     * long enough that a serialized scheduler cannot finish it silently */
    unsigned long long iters = (unsigned long long)ms * 200000ull;
    for (unsigned long long i = 0; i < iters; i++) sink++;
}

/* ── scenario B ── */
static atomic_int stop_b;
static void feeder_b(void)
{
    while (!atomic_load(&stop_b)) atomic_fetch_add(&prog, 1);
}
static void main_b(void)
{
    ui_Go(feeder_b);
    int before = atomic_load(&prog);
    /* Hold the vCPU WITHOUT blocking for well past the idle-peer steal
     * poll interval (~1ms).  Any progress here proves another thread ran
     * the feeder concurrently; on pre-fix code this window closes with
     * zero progress (feeder sits unstealable in our own runq). */
    spin_ms(SPIN_MS);
    int after = atomic_load(&prog);
    if (after - before < 1000) {
        fprintf(stderr,
                "FAIL B: lone feeder not stolen during %ums busy window "
                "(progress %d -> %d)\n",
                SPIN_MS, before, after);
        exit(1);
    }
    printf("PASS B: lone feeder progressed (%d -> %d)\n",
           before, after);
    /* let the feeder exit so the runtime can drain */
    atomic_store(&stop_b, 1);
    ui_SleepUs(5000);
}

/* ── scenario A ── */
static void feeder_a(void)
{
    spin_ms(20);
    int v = 1;
    ui_ChanSend(chan_a, &v);
    atomic_store(&done_a, 1);
}
static void main_a(void)
{
    chan_a = ui_NewChan(sizeof(int), 0);
    ui_Go(feeder_a);
    int v = 0;
    ui_ChanRecv(chan_a, &v); /* parks instantly (unbuffered) */
    if (!atomic_load(&done_a)) { fprintf(stderr, "FAIL A\n"); exit(2); }
    printf("PASS A: spawn + park resumed via handoff\n");
}

int main(void)
{
    if (ui_Init() != 0) return 3;
    int nv = ui_NVCPUs();
    printf("nvcpus=%d\n", nv);
    if (nv <= 1) {
        printf("SKIP: nvcpus==1 is sequential by contract\n");
        return 0;
    }
    fflush(stdout);

    ui_Go(main_b);
    ui_Go(main_a);
    ui_Run();
    return 0;
}
