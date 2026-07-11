#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Ticket spinlock (used by Cond and RWLock) ── */

static void
spin_lock(atomic_int *l)
{
    while (atomic_exchange(l, 1))
        __builtin_ia32_pause();
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
 * Lock fast path: CAS 0 → LOCKED
 * Unlock fast path: fetch_sub LOCKED → if old == LOCKED, done.
 *
 * Lock contention: under splock, set WAITING, re-check LOCKED.
 *   If LOCKED not set (unlock raced ahead), acquire atomically.
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

/* Fast lock: try atomic CAS first, only park on contention */
void
ui_MutexLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    /* Fast path: try to acquire without parking */
    if (atomic_compare_exchange_strong_explicit(&m->state, &(int){0}, MUTEX_LOCKED,
                                                 memory_order_acquire, memory_order_relaxed))
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
    int old = atomic_fetch_or_explicit(&m->state, MUTEX_WAITING,
                                        memory_order_relaxed);

    if (!(old & MUTEX_LOCKED))
    {
        /* Lock was released before we set WAITING. Current state is
         * old|WAITING = 2 (or 2 if old was 2). Acquire atomically:
         * CAS from WAITING-only to LOCKED.
         *
         * IMPORTANT: preserve WAITING if there are other goroutines
         * still on the waitq, so the unlock path wakes them. */
        int has_other_waiters = (m->waitq.head != NULL);
        int new_state = MUTEX_LOCKED | (has_other_waiters ? MUTEX_WAITING : 0);

        int expected = old | MUTEX_WAITING;  /* always 2 */
        if (atomic_compare_exchange_strong_explicit(&m->state, &expected,
                                                     new_state,
                                                     memory_order_acquire,
                                                     memory_order_relaxed))
        {
            spin_unlock(&m->splock);
            return;  /* Got the lock without parking */
        }
        /* CAS failed — state changed under us (e.g. another waiter
         * also set WAITING). Fall through to waitq. */
    }

    /* LOCKED is still set (or CAS failed) — actually park */
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

    int expected = 0;
    return atomic_compare_exchange_strong_explicit(&m->state, &expected, MUTEX_LOCKED,
                                                     memory_order_acquire, memory_order_relaxed);
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

/* ── Read-Write Lock ── */

typedef struct
{
    atomic_int splock;
    int         readers;
    int         writer;
    int         write_waiters;
    ui_WaitQ    read_wait;
    ui_WaitQ    write_wait;
} ui_rwlock;

uint64_t
ui_RwLockNew(void)
{
    ui_rwlock *rw = calloc(1, sizeof(ui_rwlock));
    if (rw) {
        ui_waitq_init(&rw->read_wait);
        ui_waitq_init(&rw->write_wait);
    }
    return (uint64_t)(uintptr_t)rw;
}

void
ui_RwLockRLock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    for (;;)
    {
        spin_lock(&rw->splock);
        if (!rw->writer && rw->write_waiters == 0)
        {
            rw->readers++;
            spin_unlock(&rw->splock);
            return;
        }
        {
            ui_vCPU *v = ui_this_vcpu;
            ui_Goro *g = v ? v->current : NULL;
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

    spin_lock(&rw->splock);
    rw->readers--;
    if (rw->readers == 0 && rw->write_waiters > 0)
        ui_waitq_wake_one(&rw->write_wait);
    spin_unlock(&rw->splock);
}

void
ui_RwLockWLock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    for (;;)
    {
        spin_lock(&rw->splock);
        if (rw->readers == 0 && !rw->writer)
        {
            rw->writer = 1;
            spin_unlock(&rw->splock);
            return;
        }
        rw->write_waiters++;
        {
            ui_vCPU *v = ui_this_vcpu;
            ui_Goro *g = v ? v->current : NULL;
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
}

void
ui_RwLockWUnlock(uint64_t rwh)
{
    ui_rwlock *rw = (ui_rwlock *)(uintptr_t)rwh;
    if (!rw) return;

    spin_lock(&rw->splock);
    rw->writer = 0;
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
    free((void *)(uintptr_t)rwh);
}
