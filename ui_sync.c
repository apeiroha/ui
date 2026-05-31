#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    int      locked;
    ui_WaitQ waitq;
} ui_mutex;

typedef struct
{
    ui_WaitQ waitq;
} ui_cond;

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
ui_wait(ui_Goro *g)
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
        if (!m->locked)
        {
            m->locked = 1;
            return;
        }
        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current) return;
            ui_waitq_push(&m->waitq, v->current);
            ui_wait(v->current);
        }
    }
}

bool
ui_MutexTryLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return false;
    if (!m->locked) { m->locked = 1; return true; }
    return false;
}

void
ui_MutexUnlock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m) return;
    if (ui_waitq_empty(&m->waitq))
        m->locked = 0;
    else
        ui_waitq_wake_one(&m->waitq);
}

void
ui_MutexFree(uint64_t mh)
{
    free((void *)(uintptr_t)mh);
}

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

    /* Release mutex */
    if (ui_waitq_empty(&m->waitq))
        m->locked = 0;
    else
        ui_waitq_wake_one(&m->waitq);

    /* Block on cond */
    {
        ui_vCPU *v = ui_get_vcpu();
        if (!v || !v->current) return;
        ui_waitq_push(&c->waitq, v->current);
        ui_wait(v->current);
    }

    /* Re-acquire mutex */
    for (;;)
    {
        if (!m->locked)
        {
            m->locked = 1;
            return;
        }
        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current) return;
            ui_waitq_push(&m->waitq, v->current);
            ui_wait(v->current);
        }
    }
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
