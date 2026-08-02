#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Ticket spinlock (used by Cond and RWLock) ── */

static void
spin_lock(atomic_int *l)
{
    int backoff = 1;
    while (atomic_exchange(l, 1))
    {
        for (int i = 0; i < backoff; i++)
            __builtin_ia32_pause();
        if (backoff < 64)
            backoff <<= 1;
    }
}

static void
spin_unlock(atomic_int *l)
{
    atomic_store(l, 0);
}

/* ── Optimized Mutex using atomic operations ──
 *
 * State encoding (single atomic int):
 *   bit 0 (MUTEX_LOCKED)  — lock is held
 *   bit 1 (MUTEX_WAITING) — one or more goroutines are waiting on waitq
 *
 * Lock fast path: fetch_or(state, LOCKED); if old had no LOCKED → acquired.
 *   Unlike CAS(0→LOCKED), this succeeds even when WAITING is set
 *   (e.g. after a goroutine is woken from waitq and retries).
 *
 * Unlock fast path: fetch_sub(state, LOCKED); if old == LOCKED → done.
 *
 * Lock contention: under splock, set WAITING, re-check LOCKED.
 *   If LOCKED not set (unlock raced ahead), set LOCKED with fetch_or.
 *   If LOCKED still set, push to waitq and park.
 *
 * Unlock slow path (WAITING was set): under splock, wake one waiter.
 */

#define MUTEX_LOCKED  1
#define MUTEX_WAITING 2

typedef struct
{
    atomic_int state;
    atomic_int splock;  /* protects waitq */
    ui_WaitQ    waitq;
} ui_mutex;

uint64_t
ui_MutexNew(void)
{
    ui_mutex *m = calloc(1, sizeof(ui_mutex));
    if (m) ui_waitq_init(&m->waitq);
    return (uint64_t)(uintptr_t)m;
}

/* Fast lock: try fetch_or first, only park on contention */
void
ui_MutexLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    /* Fast path: atomically set LOCKED; succeed if LOCKED was not set.
     * Using fetch_or instead of CAS(0→LOCKED) allows acquisition even
     * when the WAITING flag is set (common after wake-from-waitq). */
    int old = atomic_fetch_or_explicit(&m->state, MUTEX_LOCKED,
                                        memory_order_acquire);
    if (!(old & MUTEX_LOCKED))
        return;  /* Got it! */

    /* Contended: park goroutine - protect waitq with spinlock */
    ui_vCPU *v = ui_this_vcpu;
    ui_Goro *g = v ? v->current : NULL;
    if (!v || !g) return;

    g->state = UI_WAITING;
    spin_lock(&m->splock);

    /* Under splock: set WAITING and re-check LOCKED.
     * The splock serializes with the unlock slow path; the unlock fast
     * path (fetch_sub returning MUTEX_LOCKED) means no WAITING existed
     * when it ran, so no concurrent unlocker is in the slow path. */
    old = atomic_fetch_or_explicit(&m->state, MUTEX_WAITING,
                                    memory_order_relaxed);

    if (!(old & MUTEX_LOCKED))
    {
        /* Lock was released before we set WAITING.  Current state is
         * old|WAITING (no LOCKED).  Acquire by setting LOCKED.
         * fetch_or returns the value before addition; if old had no
         * LOCKED, we successfully acquired. */
        int prev = atomic_fetch_or_explicit(&m->state, MUTEX_LOCKED,
                                             memory_order_relaxed);
        if (!(prev & MUTEX_LOCKED))
        {
            /* Got the lock without parking.  We set the WAITING hint above
             * but never parked — clear it if there are no real waiters,
             * otherwise every future unlock takes the slow path and
             * hammers splock.  We hold splock, so the waitq is stable. */
            if (!m->waitq.head)
                atomic_fetch_and_explicit(&m->state, ~MUTEX_WAITING,
                                           memory_order_relaxed);
            spin_unlock(&m->splock);
            return;  /* Got the lock without parking */
        }
        /* Someone else acquired before our fetch_or(LOCKED) — park. */
    }

    /* LOCKED is still set — actually park */
    ui_waitq_push(&m->waitq, g);
    spin_unlock(&m->splock);
    ui_switch(&g->rsp, v->sched_rsp);
    /* Woken up — retry */
    ui_MutexLock(mh);
}

bool
ui_MutexTryLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return false;

    int old = atomic_fetch_or_explicit(&m->state, MUTEX_LOCKED,
                                        memory_order_acquire);
    return !(old & MUTEX_LOCKED);
}

void
ui_MutexUnlock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    /* Fast path: atomically release lock and check for waiters.
     * fetch_sub with release returns the state BEFORE the subtraction.
     * If old == MUTEX_LOCKED (just LOCKED, no WAITING) → done. */
    int old = atomic_fetch_sub_explicit(&m->state, MUTEX_LOCKED,
                                         memory_order_release);
    if (old == MUTEX_LOCKED)
        return;  /* No waiters, fast path */

    /* Has (or might have) waiters: wake one under splock */
    spin_lock(&m->splock);
    if (m->waitq.head)
    {
        ui_waitq_wake_one(&m->waitq);
        if (!m->waitq.head)
            atomic_fetch_and_explicit(&m->state, ~MUTEX_WAITING,
                                       memory_order_relaxed);
    }
    else
    {
        /* Stale WAITING flag with no actual waiters — clear it */
        atomic_fetch_and_explicit(&m->state, ~MUTEX_WAITING,
                                   memory_order_relaxed);
    }
    spin_unlock(&m->splock);
}

void
ui_MutexFree(uint64_t mh)
{
    free((void *)(uintptr_t)mh);
}

/* ── Condition variable ── */

typedef struct
{
    atomic_int splock;
    ui_WaitQ waitq;
} ui_cond;

uint64_t
ui_CondNew(void)
{
    ui_cond *c = calloc(1, sizeof(ui_cond));
    if (c) ui_waitq_init(&c->waitq);
    return (uint64_t)(uintptr_t)c;
}

void
ui_CondWait(uint64_t ch, uint64_t mh)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!c || !m) return;

    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return;
    ui_Goro *g = v->current;

    /* Queue on the cond before releasing the mutex to avoid lost wakeups. */
    spin_lock(&c->splock);

    /* Release mutex - atomic unlock with wakeup if waiters */
    spin_lock(&m->splock);
    atomic_fetch_and_explicit(&m->state, ~MUTEX_LOCKED, memory_order_release);
    if (m->waitq.head)
    {
        ui_waitq_wake_one(&m->waitq);
        if (!m->waitq.head)
            atomic_fetch_and_explicit(&m->state, ~MUTEX_WAITING, memory_order_relaxed);
    }
    else
    {
        atomic_fetch_and_explicit(&m->state, ~MUTEX_WAITING, memory_order_relaxed);
    }
    spin_unlock(&m->splock);

    g->state = UI_WAITING;
    ui_waitq_push(&c->waitq, g);
    spin_unlock(&c->splock);
    ui_switch(&g->rsp, v->sched_rsp);

    /* Re-acquire mutex */
    ui_MutexLock(mh);
}

void
ui_CondSignal(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c) return;
    spin_lock(&c->splock);
    ui_waitq_wake_one(&c->waitq);
    spin_unlock(&c->splock);
}

void
ui_CondBroadcast(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c) return;
    spin_lock(&c->splock);
    ui_waitq_wake_all(&c->waitq);
    spin_unlock(&c->splock);
}

void
ui_CondFree(uint64_t ch)
{
    free((void *)(uintptr_t)ch);
}

/* ── Read-Write Lock (per-vCPU sharded reader count) ──
 *
 * Reader count is sharded per vCPU: RLock/RUnlock touch ONLY the caller's
 * shard, which is its own cache line (64B stride), so the uncontended
 * read path is lock-free and never writes a line shared with other vCPUs.
 *
 * Correctness (same argument as Go's sync.RWMutex):
 *   - reader fast path:  shard++ (seq_cst) → double-check writer (acquire)
 *   - writer:            writer=1 (seq_cst) → scan all shards (acquire)
 * A reader whose double-check saw writer==0 published its shard++ before
 * the writer's store in the seq_cst total order, so the writer's scan
 * (which happens after the store) is guaranteed to observe it.  A reader
 * that sees writer==1 retracts and takes the slow path.  Hence the writer
 * may enter only once the shard sum is zero — no reader is inside.
 *
 * The goro records its shard at RLock so RUnlock decrements the same
 * shard even if the goro migrates between the two. */

typedef struct
{
    atomic_int splock;
    atomic_int writer;           /* 1 = write lock held (by the claimer) */
    int         claimed;         /* a writer holds writer=1 and waits in claim_wait for readers to drain */
    atomic_int  write_waiters;   /* writers queued behind the claim */
    ui_WaitQ    read_wait;
    ui_WaitQ    write_wait;      /* queued writers (awaiting the lock) */
    ui_WaitQ    claim_wait;      /* the claiming writer (awaiting zero readers) */
    int         nshards;
    int          *shards;    /* nshards × 64B, one cache line per shard */
} ui_rwlock;

uint64_t
ui_RwLockNew(void)
{
    ui_rwlock *rw = calloc(1, sizeof(ui_rwlock));
    if (!rw) return 0;
    int n = g_ui_sched.nvcpus > 0 ? g_ui_sched.nvcpus : 1;
    rw->nshards = n;
    rw->shards = calloc((size_t)n, 64);
    if (!rw->shards) { free(rw); return 0; }
    ui_waitq_init(&rw->read_wait);
    ui_waitq_init(&rw->write_wait);
    ui_waitq_init(&rw->claim_wait);
    return (uint64_t)(uintptr_t)rw;
}

static int
ui_rwlock_scan(ui_rwlock *rw)
{
    int total = 0;
    for (int i = 0; i < rw->nshards; i++)
        total += __atomic_load_n(&rw->shards[i], __ATOMIC_ACQUIRE);
    return total;
}

/* Reader shard update: the shard is a private cache line (no contention),
 * so the seq_cst RMW is uncontended — its cost is the lock prefix, not
 * cache-line bouncing.  The seq_cst ordering is what makes a reader's
 * count visible to a claiming writer's scan (and vice versa). */
static inline void
ui_rwlock_shard_add(ui_rwlock *rw, int sh, int delta)
{
    __atomic_fetch_add(&rw->shards[sh], delta, __ATOMIC_SEQ_CST);
}

static int
ui_rwlock_shard_of(ui_rwlock *rw, ui_vCPU *v, ui_Goro *g)
{
    int sh = v ? v->id : 0;
    if (sh >= rw->nshards) sh = 0;
    if (g) g->rwlock_shard = sh;
    return sh;
}

void
ui_RwLockRLock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    ui_vCPU *v = ui_this_vcpu;
    ui_Goro *g = v ? v->current : NULL;
    int sh = ui_rwlock_shard_of(rw, v, g);

    /* Fast path: no writer → count on our shard only.  The seq_cst shard++
     * is uncontended (private cache line); the writer load is a shared-line
     * READ (cheap, no bus lock).  write_waiters>0 implies a writer exists,
     * so the single writer check covers both. */
    if (!atomic_load_explicit(&rw->writer, memory_order_acquire))
    {
        ui_rwlock_shard_add(rw, sh, 1);
        if (!atomic_load_explicit(&rw->writer, memory_order_acquire))
            return;   /* got the read lock */
        /* A writer claimed between our check and our count.  Retract and
         * treat the retraction like an RUnlock: the claimer's scan may
         * have seen our count, so if we just drained the readers, wake it.
         * Otherwise nobody would (we never held the lock → no RUnlock). */
        ui_rwlock_shard_add(rw, sh, -1);
        if (atomic_load_explicit(&rw->writer, memory_order_acquire))
        {
            spin_lock(&rw->splock);
            if (rw->claimed && ui_rwlock_scan(rw) == 0)
                ui_waitq_wake_one(&rw->claim_wait);
            spin_unlock(&rw->splock);
        }
    }

    /* Slow path: writer present or contended. */
    for (;;)
    {
        v = ui_this_vcpu;
        g = v ? v->current : NULL;
        sh = ui_rwlock_shard_of(rw, v, g);
        spin_lock(&rw->splock);
        if (!rw->writer && rw->write_waiters == 0)
        {
            ui_rwlock_shard_add(rw, sh, 1);
            spin_unlock(&rw->splock);
            return;
        }
        {
            if (v && g)
            {
                g->state = UI_WAITING;
                ui_waitq_push(&rw->read_wait, g);
                spin_unlock(&rw->splock);
                ui_switch(&g->rsp, v->sched_rsp);
            }
            else
            {
                spin_unlock(&rw->splock);
                return;
            }
        }
    }
}

void
ui_RwLockRUnlock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    ui_vCPU *v = ui_this_vcpu;
    ui_Goro *g = v ? v->current : NULL;
    int sh = (g && g->rwlock_shard >= 0 && g->rwlock_shard < rw->nshards)
                 ? g->rwlock_shard
                 : (v ? v->id : 0);

    ui_rwlock_shard_add(rw, sh, -1);

    /* No writer claiming → done (common case, zero extra cost).  A queued
     * writer (write_waiters>0) is woken by WUnlock, not by readers. */
    if (!atomic_load_explicit(&rw->writer, memory_order_acquire))
        return;

    /* A writer may be waiting: wake the claimer if we were the last reader. */
    spin_lock(&rw->splock);
    if (rw->claimed && ui_rwlock_scan(rw) == 0)
        ui_waitq_wake_one(&rw->claim_wait);
    spin_unlock(&rw->splock);
}

void
ui_RwLockWLock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    int resuming = 0;
    for (;;)
    {
        ui_vCPU *v = ui_this_vcpu;
        ui_Goro *g = v ? v->current : NULL;

        spin_lock(&rw->splock);

        if (resuming)
        {
            /* We claimed writer=1 earlier and were parked in claim_wait;
             * the write claim is still ours (no one else can hold it).
             * Re-scan: if readers have drained, acquire and return. */
            if (ui_rwlock_scan(rw) == 0)
            {
                rw->claimed = 0;
                spin_unlock(&rw->splock);
                return;   /* got the write lock */
            }
            if (v && g)
            {
                g->state = UI_WAITING;
                ui_waitq_push(&rw->claim_wait, g);
                spin_unlock(&rw->splock);
                ui_switch(&g->rsp, v->sched_rsp);
            }
            else
            {
                spin_unlock(&rw->splock);
                return;
            }
            continue;
        }

        if (rw->claimed)
        {
            /* Another writer holds the claim and waits for readers. */
            rw->write_waiters++;
            if (v && g)
            {
                g->state = UI_WAITING;
                ui_waitq_push(&rw->write_wait, g);
                spin_unlock(&rw->splock);
                ui_switch(&g->rsp, v->sched_rsp);
            }
            else
            {
                spin_unlock(&rw->splock);
                return;
            }
            continue;
        }

        if (!rw->writer && !rw->claimed)
        {
            /* Acquire the write claim first (seq_cst) so new readers either
             * see it (slow path) or their count is guaranteed visible to
             * our scan below (seq_cst total order). */
            atomic_store_explicit(&rw->writer, 1, memory_order_seq_cst);
            if (ui_rwlock_scan(rw) == 0)
            {
                spin_unlock(&rw->splock);
                return;   /* got the write lock */
            }
            /* Readers active: park while holding the claim; RUnlock wakes
             * us (via claim_wait) when the last reader exits. */
            rw->claimed = 1;
            resuming = 1;
            if (v && g)
            {
                g->state = UI_WAITING;
                ui_waitq_push(&rw->claim_wait, g);
                spin_unlock(&rw->splock);
                ui_switch(&g->rsp, v->sched_rsp);
            }
            else
            {
                spin_unlock(&rw->splock);
                return;
            }
            continue;
        }

        /* Write lock held by someone else (claiming or holding): queue up. */
        rw->write_waiters++;
        if (v && g)
        {
            g->state = UI_WAITING;
            ui_waitq_push(&rw->write_wait, g);
            spin_unlock(&rw->splock);
            ui_switch(&g->rsp, v->sched_rsp);
        }
        else
        {
            spin_unlock(&rw->splock);
            return;
        }
    }
}

void
ui_RwLockWUnlock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    spin_lock(&rw->splock);
    atomic_store_explicit(&rw->writer, 0, memory_order_release);
    rw->claimed = 0;   /* defensive: claim is always cleared on acquire */
    if (rw->write_waiters > 0)
    {
        rw->write_waiters--;
        ui_waitq_wake_one(&rw->write_wait);
    }
    else
    {
        ui_waitq_wake_all(&rw->read_wait);
    }
    spin_unlock(&rw->splock);
}

void
ui_RwLockFree(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (rw)
    {
        free(rw->shards);
        free(rw);
    }
}
