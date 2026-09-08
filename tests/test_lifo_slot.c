/* Regression tests for the per-vCPU LIFO slot (runnext analog).
 *
 * C1 handoff re-convergence: an unbuffered-chan ping-pong pair may be
 *    split across vCPUs by stealing at spawn time, but every direct
 *    handoff places the partner on the waker's LIFO slot (Go
 *    goready(next=true) analog), so after some messages both sides
 *    must converge onto ONE thread and stay there.
 *
 * C2 anti-monopoly cap: a hot communicate-then-run pair must not
 *    starve independent work queued behind it — UI_RUNNEXT_MAX_POLLS
 *    consecutive slot schedules demote the occupant to the FIFO tail,
 *    letting the background goro make progress (ui has no preemption;
 *    this cap is the only guard).
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

static atomic_int pair_done;
static atomic_int rx2_done;
static void pp_tx2_entry(void);

#define PP_MSGS 20000

static uint64_t ch;
static atomic_long tx_tid, rx_tid;
static atomic_int pp_done;

static void pp_tx(void)
{
    for (int64_t i = 0; i < PP_MSGS; i++) {
        int64_t v = i;
        ui_ChanSend(ch, &v);
    }
    atomic_store(&tx_tid, (long)gettid_());
}

static void pp_rx(void)
{
    int64_t v;
    for (int i = 0; i < PP_MSGS; i++) ui_ChanRecv(ch, &v);
    atomic_store(&rx_tid, (long)gettid_());
    atomic_store(&pp_done, 1);
}

static void stage_c1(void)
{
    ch = ui_NewChan(sizeof(int64_t), 0);
    ui_Go(pp_tx);
    ui_Go(pp_rx);
    int ms = 0;
    while (!atomic_load(&pp_done)) { ui_SleepUs(1000); if (++ms > 30000) break; }
    /* ensure both partners fully exited before global chan reuse */
    while (!atomic_load(&tx_tid)) { ui_SleepUs(1000); }
    ui_SleepUs(5000);
    long t = (long)atomic_load(&tx_tid), r = (long)atomic_load(&rx_tid);
    printf("C1 handoff re-convergence: tx_tid=%ld rx_tid=%ld %s\n",
           t, r, t == r && t != 0 ? "CONVERGED" : "split");
    if (!(t == r && t != 0)) exit(1);
}

/* ── C2 ── */
#define BG_TARGET 20000000
static atomic_int bg_prog;
static atomic_int bg_ok;

static void bg_worker(void)
{
    while (atomic_load(&bg_prog) < BG_TARGET)
        atomic_fetch_add(&bg_prog, 1);
    atomic_store(&bg_ok, 1);
}

static void stage_c2(void)
{
    /* wait for C1 teardown */
    ui_SleepUs(20000);

    atomic_store(&bg_prog, 0);
    atomic_store(&bg_ok, 0);
    ui_Go(bg_worker);

    /* hot pair hammering an unbuffered channel */
    ch = ui_NewChan(sizeof(int64_t), 0);
    fprintf(stderr, "[c2] new ch=%llu\n", (unsigned long long)ch);
    atomic_store(&pair_done, 0);
    ui_Go(pp_tx2_entry);

    /* background must reach target within deadline despite the pair */
    int waited_ms = 0;
    while (!atomic_load(&bg_ok)) {
        ui_SleepUs(1000);
        if (++waited_ms > 15000) break;
    }
    if (!atomic_load(&bg_ok)) {
        fprintf(stderr, "FAIL C2: background starved by LIFO pair "
                "(progress %d / %d)\n",
                atomic_load(&bg_prog), BG_TARGET);
        exit(2);
    }
    printf("C2 anti-monopoly: background finished in %d ms beside hot pair\n",
           waited_ms);

    /* tear the bounded pair down: channel is capacity-1 buffered so tx
     * may finish long before a lagging rx — wait for BOTH before
     * closing, else rx's reply-send hits the closed channel. */
    {
        int ms = 0;
        while ((!atomic_load(&pair_done) || !atomic_load(&rx2_done)) &&
               ms < 30000) { ui_SleepUs(1000); ms++; }
        if (!atomic_load(&pair_done) || !atomic_load(&rx2_done)) {
            fprintf(stderr, "FAIL C2 teardown: pair stuck "
                    "(pair=%d rx=%d)\n",
                    atomic_load(&pair_done), atomic_load(&rx2_done));
            exit(4);
        }
    }
    ui_SleepUs(5000);
}

/* bounded hot pair for C2 — PURE roles (tx only sends, rx only recvs);
 * a single goro that both sends and receives on a capacity-1 channel
 * can consume its own message and starve the partner (test-design
 * trap, not a scheduler issue). */

static atomic_int pair_done;
static atomic_int rx2_done;

static void pp_tx2(void)
{
    fprintf(stderr, "[tx2] ch=%llu &ch=%p\n",
            (unsigned long long)ch, (void*)&ch);
    for (int i = 0; i < 300000; i++) {
        int64_t v = i;
        ui_ChanSend(ch, &v);
    }
    atomic_store(&pair_done, 1);
}

static void pp_rx2(void)
{
    int64_t v;
    for (int i = 0; i < 300000; i++) ui_ChanRecv(ch, &v);
    atomic_store(&rx2_done, 1);
}

static void pp_tx2_entry(void)
{
    ui_Go(pp_rx2);
    pp_tx2();
}

int main(void)
{
    if (ui_Init() != 0) return 3;
    printf("nvcpus=%d\n", ui_NVCPUs());
    fflush(stdout);
    ui_Go(stage_c1);
    ui_Go(stage_c2);
    ui_Run();
    return 0;
}
