#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Ticket spinlock ── */

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

/* ── Mutex ── */

typedef struct
{
    atomic_int splock;
    int         locked;
    ui_WaitQ    waitq;
} ui_mutex;

static ui_vCPU *
ui_get_vcpu(void)
{
    return ui_this_vcpu;
}

uint64_t
ui_MutexNew(void)
{
    ui_mutex *m = calloc(1, sizeof(ui_mutex));
    if (m) ui_waitq_init(&m->waitq);
    return (uint64_t)(uintptr_t)m;
}

void
ui_MutexLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    for (;;)
    {
        spin_lock(&m->splock);
        if (!m->locked)
        {
            m->locked = 1;
            spin_unlock(&m->splock);
            return;
        }
        {
            ui_vCPU *v = ui_get_vcpu();
            ui_Goro *g = v ? v->current : NULL;
            if (v && g)
            {
                g->state = UI_WAITING;
                ui_waitq_push(&m->waitq, g);
                spin_unlock(&m->splock);
                ui_switch(&g->rsp, v->sched_rsp);
                /* Woken up — retry */
            }
            else
            {
                spin_unlock(&m->splock);
                return;
            }
        }
    }
}

bool
ui_MutexTryLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return false;
    spin_lock(&m->splock);
    if (!m->locked) { m->locked = 1; spin_unlock(&m->splock); return true; }
    spin_unlock(&m->splock);
    return false;
}

void
ui_MutexUnlock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;

    spin_lock(&m->splock);
    m->locked = 0;
    if (!ui_waitq_empty(&m->waitq))
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

    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    ui_Goro *g = v->current;

    /* Queue on the cond before releasing the mutex to avoid lost wakeups. */
    spin_lock(&c->splock);

    /* Release mutex (under its spinlock to prevent race) */
    spin_lock(&m->splock);
    m->locked = 0;
    if (!ui_waitq_empty(&m->waitq))
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
            ui_vCPU *v = ui_get_vcpu();
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
            ui_vCPU *v = ui_get_vcpu();
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
