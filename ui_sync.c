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
    pthread_t self = pthread_self();
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        if (pthread_equal(g_ui_sched.vcpus[i].thread, self))
            return &g_ui_sched.vcpus[i];
    }
    return NULL;
}

static void
ui_wait_g(ui_Goro *g)
{
    g->state = UI_WAITING;
    ui_vCPU *v = ui_get_vcpu();
    if (v) ui_switch(&g->rsp, v->sched_rsp);
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
        /* Under spinlock: push to waitq, then release and sleep */
        if (ui_get_vcpu() && ui_get_vcpu()->current)
        {
            ui_waitq_push(&m->waitq, ui_get_vcpu()->current);
            spin_unlock(&m->splock);
            ui_wait_g(ui_get_vcpu()->current);
            /* Woken up — retry */
        }
        else
        {
            spin_unlock(&m->splock);
            return;
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

    /* Release mutex (under its spinlock to prevent race) */
    spin_lock(&m->splock);
    m->locked = 0;
    if (!ui_waitq_empty(&m->waitq))
        ui_waitq_wake_one(&m->waitq);
    spin_unlock(&m->splock);

    /* Block on cond */
    {
        ui_vCPU *v = ui_get_vcpu();
        if (!v || !v->current) return;
        ui_waitq_push(&c->waitq, v->current);
        ui_wait_g(v->current);
    }

    /* Re-acquire mutex */
    ui_MutexLock(mh);
}

void
ui_CondSignal(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c) return;
    ui_waitq_wake_one(&c->waitq);
}

void
ui_CondBroadcast(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c) return;
    ui_waitq_wake_all(&c->waitq);
}

void
ui_CondFree(uint64_t ch)
{
    free((void *)(uintptr_t)ch);
}
