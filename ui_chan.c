#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct ui_Chan ui_Chan;

struct ui_Chan
{
    void    *buf;
    size_t   elem_size;
    unsigned cap;
    unsigned count;
    unsigned read_idx;
    unsigned write_idx;
    int      closed;

    ui_Goro *send_wait;
    int      send_count;
    ui_Goro *recv_wait;
    int      recv_count;
};

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
ui_wait(ui_Goro *g, int new_state)
{
    g->state = new_state;
    ui_vCPU *v = ui_get_vcpu();
    if (v)
        ui_switch(&g->rsp, v->sched_rsp);
}

static void
ui_wake_one(ui_Goro **list, int *count)
{
    if (*count == 0)
        return;

    ui_Goro *w = *list;
    *list = w->wait_next;
    (*count)--;
    w->wait_next = NULL;
    w->state = UI_READY;
    ui_wakeup(w);
}

uint64_t
ui_NewChan(size_t elem_size, unsigned int buf_cap)
{
    ui_Chan *c = calloc(1, sizeof(ui_Chan));
    if (!c)
        return 0;

    if (buf_cap == 0)
        buf_cap = 1;

    c->elem_size = elem_size;
    c->cap = buf_cap + 1;
    c->buf = calloc(c->cap, elem_size);
    if (!c->buf)
    {
        free(c);
        return 0;
    }

    return (uint64_t)(uintptr_t)c;
}

void
ui_ChanSend(uint64_t ch, const void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c || c->closed)
        return;

    for (;;)
    {
        if (c->count < c->cap - 1)
        {
            unsigned pos = c->write_idx % c->cap;
            memcpy((char *)c->buf + pos * c->elem_size, val, c->elem_size);
            c->write_idx++;
            c->count++;

            ui_wake_one(&c->recv_wait, &c->recv_count);
            return;
        }

        /* Full — block */
        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current)
                return;

            ui_Goro *cur = v->current;
            cur->wait_next = NULL;
            if (!c->send_wait)
                c->send_wait = cur;
            else
            {
                ui_Goro *p = c->send_wait;
                while (p->wait_next)
                    p = p->wait_next;
                p->wait_next = cur;
            }
            c->send_count++;

            ui_wait(cur, UI_WAITING);

            if (c->closed)
                return;
        }
    }
}

void
ui_ChanRecv(uint64_t ch, void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c)
        return;

    for (;;)
    {
        if (c->count > 0)
        {
            unsigned pos = c->read_idx % c->cap;
            if (val)
                memcpy(val, (char *)c->buf + pos * c->elem_size, c->elem_size);
            c->read_idx++;
            c->count--;

            ui_wake_one(&c->send_wait, &c->send_count);
            return;
        }

        if (c->closed)
        {
            if (val)
                memset(val, 0, c->elem_size);
            return;
        }

        /* Empty — block */
        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current)
                return;

            ui_Goro *cur = v->current;
            cur->wait_next = NULL;
            if (!c->recv_wait)
                c->recv_wait = cur;
            else
            {
                ui_Goro *p = c->recv_wait;
                while (p->wait_next)
                    p = p->wait_next;
                p->wait_next = cur;
            }
            c->recv_count++;

            ui_wait(cur, UI_WAITING);

            if (c->closed && c->count == 0)
            {
                if (val)
                    memset(val, 0, c->elem_size);
                return;
            }
        }
    }
}

bool
ui_ChanTrySend(uint64_t ch, const void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c || c->closed)
        return false;

    if (c->count < c->cap - 1)
    {
        unsigned pos = c->write_idx % c->cap;
        memcpy((char *)c->buf + pos * c->elem_size, val, c->elem_size);
        c->write_idx++;
        c->count++;

        ui_wake_one(&c->recv_wait, &c->recv_count);
        return true;
    }
    return false;
}

bool
ui_ChanTryRecv(uint64_t ch, void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c)
        return false;

    if (c->closed)
    {
        if (val)
            memset(val, 0, c->elem_size);
        return true;
    }

    if (c->count > 0)
    {
        unsigned pos = c->read_idx % c->cap;
        if (val)
            memcpy(val, (char *)c->buf + pos * c->elem_size, c->elem_size);
        c->read_idx++;
        c->count--;

        ui_wake_one(&c->send_wait, &c->send_count);
        return true;
    }
    return false;
}

void
ui_ChanClose(uint64_t ch)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c || c->closed)
        return;

    c->closed = 1;

    while (c->send_wait)
    {
        ui_Goro *w = c->send_wait;
        c->send_wait = w->wait_next;
        c->send_count--;
        w->wait_next = NULL;
        w->state = UI_READY;
        ui_wakeup(w);
    }

    while (c->recv_wait)
    {
        ui_Goro *w = c->recv_wait;
        c->recv_wait = w->wait_next;
        c->recv_count--;
        w->wait_next = NULL;
        w->state = UI_READY;
        ui_wakeup(w);
    }
}

void
ui_ChanFree(uint64_t ch)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c)
        return;

    ui_ChanClose(ch);
    free(c->buf);
    free(c);
}

int
ui_SelectWait(const uint64_t *recv_chs, void **recv_bufs,
              const uint64_t *send_chs, const void **send_vals,
              int nrecv, int nsend, int timeout_ms)
{
    (void)timeout_ms;

    /* Phase 1: try all non-blocking */
    for (int i = 0; i < nrecv; i++)
    {
        ui_Chan *c = (ui_Chan *)(uintptr_t)recv_chs[i];
        if (c->count > 0)
        {
            ui_ChanRecv(recv_chs[i], recv_bufs[i]);
            return i;
        }
        if (c->closed)
        {
            if (recv_bufs[i])
                memset(recv_bufs[i], 0, c->elem_size);
            return i;
        }
    }

    for (int i = 0; i < nsend; i++)
    {
        ui_Chan *c = (ui_Chan *)(uintptr_t)send_chs[i];
        if (c->count < c->cap - 1 && !c->closed)
        {
            ui_ChanTrySend(send_chs[i], send_vals[i]);
            return nrecv + i;
        }
    }

    return -1;
}
