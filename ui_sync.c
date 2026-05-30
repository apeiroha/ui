#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    int      locked;
    ui_Goro *wait_queue;
    int      wait_count;
} ui_mutex;

typedef struct
{
    ui_Goro *wait_queue;
    int      wait_count;
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
    if (v)
        ui_switch(&g->rsp, v->sched_rsp);
}

uint64_t
ui_MutexNew(void)
{
    ui_mutex *m = calloc(1, sizeof(ui_mutex));
    return (uint64_t)(uintptr_t)m;
}

void
ui_MutexLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m)
        return;

    for (;;)
    {
        if (!m->locked)
        {
            m->locked = 1;
            return;
        }

        /* Block */
        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current)
                return;

            ui_Goro *cur = v->current;
            cur->wq_next = NULL;
            if (!m->wait_queue)
                m->wait_queue = cur;
            else
            {
                ui_Goro *p = m->wait_queue;
                while (p->wq_next)
                    p = p->wq_next;
                p->wq_next = cur;
            }
            m->wait_count++;

            ui_wait(cur);
        }
    }
}

bool
ui_MutexTryLock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m)
        return false;

    if (!m->locked)
    {
        m->locked = 1;
        return true;
    }
    return false;
}

void
ui_MutexUnlock(uint64_t mh)
{
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!m)
        return;

    if (m->wait_count > 0)
    {
        ui_Goro *w = m->wait_queue;
        m->wait_queue = w->wq_next;
        m->wait_count--;
        w->wq_next = NULL;
        w->state = UI_READY;
        ui_wakeup(w);
    }

    m->locked = 0;
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
    return (uint64_t)(uintptr_t)c;
}

void
ui_CondWait(uint64_t ch, uint64_t mh)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    ui_mutex *m = (ui_mutex *)(uintptr_t)mh;
    if (!c || !m)
        return;

    /* Release mutex */
    if (m->wait_count > 0)
    {
        ui_Goro *w = m->wait_queue;
        m->wait_queue = w->wq_next;
        m->wait_count--;
        w->wq_next = NULL;
        w->state = UI_READY;
        ui_wakeup(w);
    }

    m->locked = 0;

    /* Block on cond */
    {
        ui_vCPU *v = ui_get_vcpu();
        if (!v || !v->current)
            return;

        ui_Goro *cur = v->current;
        cur->wq_next = NULL;
        if (!c->wait_queue)
            c->wait_queue = cur;
        else
        {
            ui_Goro *p = c->wait_queue;
            while (p->wq_next)
                p = p->wq_next;
            p->wq_next = cur;
        }
        c->wait_count++;

        ui_wait(cur);
    }

    /* Re-acquire mutex */
    ui_MutexLock(mh);
}

void
ui_CondSignal(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c || c->wait_count == 0)
        return;

    ui_Goro *w = c->wait_queue;
    c->wait_queue = w->wq_next;
    c->wait_count--;
    w->wq_next = NULL;
    w->state = UI_READY;
    ui_wakeup(w);
}

void
ui_CondBroadcast(uint64_t ch)
{
    ui_cond *c = (ui_cond *)(uintptr_t)ch;
    if (!c)
        return;

    while (c->wait_queue)
    {
        ui_Goro *w = c->wait_queue;
        c->wait_queue = w->wq_next;
        c->wait_count--;
        w->wq_next = NULL;
        w->state = UI_READY;
        ui_wakeup(w);
    }
}

void
ui_CondFree(uint64_t ch)
{
    free((void *)(uintptr_t)ch);
}
