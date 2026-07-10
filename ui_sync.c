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

/* ── Optimized Mutex using atomic operations ── */

typedef struct
{
    atomic_int locked;
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
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&m->locked, &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed))
        return;  /* Got it! */

    /* Contended: park goroutine - protect waitq with spinlock */
    ui_vCPU *v = ui_this_vcpu;
    ui_Goro *g = v ? v->current : NULL;
    if (!v || !g) return;

    g->state = UI_WAITING;
    spin_lock(&m->splock);
    if (!m->locked)
    {
        /* Lock became free while we were waiting for spinlock */
        m->locked = 1;
        spin_unlock(&m->splock);
        return;
    }
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
    return atomic_compare_exchange_strong_explicit(&m->locked, &expected, 1,
                                                    memory_order_acquire, memory_order_relaxed);
}

void
ui_MutexUnlock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    /* Fast path: if no waiters in queue, just release with release semantics */
    spin_lock(&m->splock);
    if (!m->waitq.head)
    {
        m->locked = 0;
        spin_unlock(&m->splock);
        return;
    }

    /* Has waiters: need to wake one */
    m->locked = 0;
    ui_waitq_wake_one(&m->waitq);
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
    atomic_store_explicit(&m->locked, 0, memory_order_release);
    if (m->waitq.head)
        ui_waitq_wake_one(&m->waitq);
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
