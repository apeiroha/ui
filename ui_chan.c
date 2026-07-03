#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

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

    pthread_spinlock_t lock;
    ui_WaitQ send_wait;
    ui_WaitQ recv_wait;
};

static ui_vCPU *
ui_get_vcpu(void)
{
    return ui_this_vcpu;
}

uint64_t
ui_NewChan(size_t elem_size, unsigned int buf_cap)
{
    ui_Chan *c = calloc(1, sizeof(ui_Chan));
    if (!c) return 0;

    if (buf_cap == 0) buf_cap = 1; /* rendezvous is approximated as capacity 1 */

    c->elem_size = elem_size;
    c->cap = buf_cap;
    c->buf = calloc(c->cap, elem_size);
    if (!c->buf) { free(c); return 0; }

    pthread_spin_init(&c->lock, PTHREAD_PROCESS_PRIVATE);
    ui_waitq_init(&c->send_wait);
    ui_waitq_init(&c->recv_wait);

    return (uint64_t)(uintptr_t)c;
}

void
ui_ChanSend(uint64_t ch, const void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return;

    for (;;)
    {
        pthread_spin_lock(&c->lock);
        if (c->closed) { pthread_spin_unlock(&c->lock); return; }

        /* Direct handoff: a recver is already waiting — bypass buffer.
         * Only safe when the wait node has no custom wake callback
         * (ChanRecv uses ui_waitq_push which sets wake=NULL;
         * SelectWait sets wake=ui_select_wake). */
        if (c->recv_wait.head && !c->recv_wait.head->wake)
        {
            ui_WaitNode *n = c->recv_wait.head;
            ui_waitq_remove_node(&c->recv_wait, n);
            if (n->g->chan_recv_ptr)
                memcpy(n->g->chan_recv_ptr, val, c->elem_size);
            n->g->chan_handoff = 1;
            ui_wakeup(n->g);
            pthread_spin_unlock(&c->lock);
            return;
        }

        if (c->count < c->cap)
        {
            unsigned pos = c->write_idx % c->cap;
            memcpy((char *)c->buf + pos * c->elem_size, val, c->elem_size);
            c->write_idx++;
            c->count++;
            ui_waitq_wake_one(&c->recv_wait);
            pthread_spin_unlock(&c->lock);
            return;
        }

        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current) { pthread_spin_unlock(&c->lock); return; }
            ui_Goro *g = v->current;
            g->chan_send_ptr = val;
            g->state = UI_WAITING;
            ui_waitq_push(&c->send_wait, g);
            pthread_spin_unlock(&c->lock);
            ui_switch(&g->rsp, v->sched_rsp);
            /* Woken: check if handoff already completed */
            if (g->chan_handoff) {
                g->chan_handoff = 0;
                return;
            }
        }
    }
}

void
ui_ChanRecv(uint64_t ch, void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return;

    for (;;)
    {
        pthread_spin_lock(&c->lock);

        /* Direct handoff: a sender is already waiting — bypass buffer.
         * Only safe when the wait node has no custom wake callback
         * (ChanSend uses ui_waitq_push which sets wake=NULL). */
        if (c->send_wait.head && !c->send_wait.head->wake)
        {
            ui_WaitNode *n = c->send_wait.head;
            ui_waitq_remove_node(&c->send_wait, n);
            const void *send_data = n->g->chan_send_ptr;
            if (send_data && val)
                memcpy(val, send_data, c->elem_size);
            n->g->chan_handoff = 1;
            ui_wakeup(n->g);
            pthread_spin_unlock(&c->lock);
            return;
        }

        if (c->count > 0)
        {
            unsigned pos = c->read_idx % c->cap;
            if (val)
                memcpy(val, (char *)c->buf + pos * c->elem_size, c->elem_size);
            c->read_idx++;
            c->count--;
            ui_waitq_wake_one(&c->send_wait);
            pthread_spin_unlock(&c->lock);
            return;
        }

        if (c->closed)
        {
            pthread_spin_unlock(&c->lock);
            if (val) memset(val, 0, c->elem_size);
            return;
        }

        {
            ui_vCPU *v = ui_get_vcpu();
            if (!v || !v->current) { pthread_spin_unlock(&c->lock); return; }
            ui_Goro *g = v->current;
            g->chan_recv_ptr = val;
            g->state = UI_WAITING;
            ui_waitq_push(&c->recv_wait, g);
            pthread_spin_unlock(&c->lock);
            ui_switch(&g->rsp, v->sched_rsp);
            /* Woken: check if handoff already completed */
            if (g->chan_handoff) {
                g->chan_handoff = 0;
                return;
            }
        }
    }
}

bool
ui_ChanTrySend(uint64_t ch, const void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return false;

    pthread_spin_lock(&c->lock);
    if (c->closed) { pthread_spin_unlock(&c->lock); return false; }

    if (c->count < c->cap)
    {
        unsigned pos = c->write_idx % c->cap;
        memcpy((char *)c->buf + pos * c->elem_size, val, c->elem_size);
        c->write_idx++;
        c->count++;
        ui_waitq_wake_one(&c->recv_wait);
        pthread_spin_unlock(&c->lock);
        return true;
    }
    pthread_spin_unlock(&c->lock);
    return false;
}

bool
ui_ChanTryRecv(uint64_t ch, void *val)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return false;

    pthread_spin_lock(&c->lock);

    if (c->count > 0)
    {
        unsigned pos = c->read_idx % c->cap;
        if (val)
            memcpy(val, (char *)c->buf + pos * c->elem_size, c->elem_size);
        c->read_idx++;
        c->count--;
        ui_waitq_wake_one(&c->send_wait);
        pthread_spin_unlock(&c->lock);
        return true;
    }

    if (c->closed)
    {
        pthread_spin_unlock(&c->lock);
        if (val) memset(val, 0, c->elem_size);
        return true;
    }

    pthread_spin_unlock(&c->lock);
    return false;
}

void
ui_ChanClose(uint64_t ch)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return;

    pthread_spin_lock(&c->lock);
    if (c->closed) { pthread_spin_unlock(&c->lock); return; }
    c->closed = 1;
    ui_waitq_wake_all(&c->send_wait);
    ui_waitq_wake_all(&c->recv_wait);
    pthread_spin_unlock(&c->lock);
}

void
ui_ChanFree(uint64_t ch)
{
    ui_Chan *c = (ui_Chan *)(uintptr_t)ch;
    if (!c) return;
    /* Caller must ensure no goroutines reference this channel
     * (typically after ui_Run() returns and all goroutines are done). */
    ui_ChanClose(ch);
    pthread_spin_destroy(&c->lock);
    memset(c->buf, 0xFD, c->cap * c->elem_size); /* debug canary */
    free(c->buf);
    memset(c, 0xFC, sizeof(ui_Chan));             /* debug canary */
    free(c);
}

// ── Timer Channel ──

typedef struct
{
    uint64_t ch;
    unsigned int us;
} ui_TimerArg;

static void
_ui_timer_proc(uintptr_t arg)
{
    ui_TimerArg *ta = (ui_TimerArg *)(uintptr_t)arg;
    uint64_t ch = ta->ch;
    unsigned int us = ta->us;
    free(ta);
    ui_SleepUs(us);
    uint64_t val = 1;
    ui_ChanSend(ch, &val);
}

uint64_t
ui_NewTimer(unsigned int ms)
{
    return ui_NewTimerUs(ms * 1000);
}

uint64_t
ui_NewTimerUs(unsigned int us)
{
    uint64_t ch = ui_NewChan(sizeof(uint64_t), 1);
    if (!ch) return 0;
    ui_TimerArg *ta = malloc(sizeof(*ta));
    if (!ta) {
        ui_ChanFree(ch);
        return 0;
    }
    ta->ch = ch;
    ta->us = us;
    ui_Go1Sized(_ui_timer_proc, (uintptr_t)ta, 0);
    return ch;
}

void
ui_TimerStop(uint64_t ch)
{
    if (ch)
        ui_ChanClose(ch);
}

uint64_t
ui_TimerReset(uint64_t old_ch, unsigned int new_us)
{
    if (old_ch)
        ui_ChanClose(old_ch);
    return ui_NewTimerUs(new_us);
}

typedef struct
{
    atomic_int fired;
} ui_SelectCtx;

static void
ui_select_wake(ui_WaitNode *n)
{
    ui_SelectCtx *ctx = (ui_SelectCtx *)n->data;
    if (!ctx) return;
    if (atomic_exchange_explicit(&ctx->fired, 1, memory_order_acq_rel) == 0)
        ui_wakeup(n->g);
}

static int
ui_select_try_recv(ui_Chan *c, void *buf)
{
    if (c->count > 0)
    {
        unsigned pos = c->read_idx % c->cap;
        if (buf)
            memcpy(buf, (char *)c->buf + pos * c->elem_size, c->elem_size);
        c->read_idx++;
        c->count--;
        ui_waitq_wake_one(&c->send_wait);
        return 1;
    }
    if (c->closed)
    {
        if (buf) memset(buf, 0, c->elem_size);
        return 1;
    }
    return 0;
}

static int
ui_select_try_send(ui_Chan *c, const void *val)
{
    if (c->count < c->cap && !c->closed)
    {
        unsigned pos = c->write_idx % c->cap;
        memcpy((char *)c->buf + pos * c->elem_size, val, c->elem_size);
        c->write_idx++;
        c->count++;
        ui_waitq_wake_one(&c->recv_wait);
        return 1;
    }
    return 0;
}

static void
ui_select_cleanup(const uint64_t *recv_chs, const uint64_t *send_chs,
                  ui_WaitNode *nodes, int nrecv, int nsend)
{
    for (int i = 0; i < nrecv; i++)
    {
        if (!nodes[i].active) continue;
        ui_Chan *c = (ui_Chan *)(uintptr_t)recv_chs[i];
        pthread_spin_lock(&c->lock);
        ui_waitq_remove_node(&c->recv_wait, &nodes[i]);
        pthread_spin_unlock(&c->lock);
    }
    for (int i = 0; i < nsend; i++)
    {
        int ni = nrecv + i;
        if (!nodes[ni].active) continue;
        ui_Chan *c = (ui_Chan *)(uintptr_t)send_chs[i];
        pthread_spin_lock(&c->lock);
        ui_waitq_remove_node(&c->send_wait, &nodes[ni]);
        pthread_spin_unlock(&c->lock);
    }
}

int
ui_SelectWait(const uint64_t *recv_chs, void **recv_bufs,
              const uint64_t *send_chs, const void **send_vals,
              int nrecv, int nsend, int timeout_ms)
{
    int total = nrecv + nsend;
    if (total <= 0)
    {
        if (timeout_ms == 0) return -1;
        if (timeout_ms > 0) ui_Sleep((unsigned)timeout_ms);
        else for (;;) ui_Sleep(1000);
        return -1;
    }

    ui_vCPU *v = ui_get_vcpu();
    ui_Goro *g = v ? v->current : NULL;
    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = ui_now_us() + (uint64_t)timeout_ms * 1000;

    for (;;)
    {
        for (int i = 0; i < nrecv; i++)
        {
            ui_Chan *c = (ui_Chan *)(uintptr_t)recv_chs[i];
            pthread_spin_lock(&c->lock);
            int ok = ui_select_try_recv(c, recv_bufs ? recv_bufs[i] : NULL);
            pthread_spin_unlock(&c->lock);
            if (ok) return i;
        }

        for (int i = 0; i < nsend; i++)
        {
            ui_Chan *c = (ui_Chan *)(uintptr_t)send_chs[i];
            pthread_spin_lock(&c->lock);
            int ok = ui_select_try_send(c, send_vals[i]);
            pthread_spin_unlock(&c->lock);
            if (ok) return nrecv + i;
        }

        if (timeout_ms == 0)
            return -1;

        if (timeout_ms > 0)
        {
            uint64_t now = ui_now_us();
            if (now >= deadline)
                return -1;
        }

        if (!v || !g)
        {
            ui_Yield();
            continue;
        }

        ui_SelectCtx ctx;
        atomic_init(&ctx.fired, 0);
        ui_WaitNode *nodes = calloc((size_t)total, sizeof(*nodes));
        if (!nodes)
            return -1;

        for (int i = 0; i < total; i++)
        {
            nodes[i].g = g;
            nodes[i].wake = ui_select_wake;
            nodes[i].data = &ctx;
        }

        for (int i = 0; i < nrecv; i++)
        {
            ui_Chan *c = (ui_Chan *)(uintptr_t)recv_chs[i];
            pthread_spin_lock(&c->lock);
            if (ui_select_try_recv(c, recv_bufs ? recv_bufs[i] : NULL))
            {
                pthread_spin_unlock(&c->lock);
                ui_select_cleanup(recv_chs, send_chs, nodes, nrecv, nsend);
                free(nodes);
                return i;
            }
            ui_waitq_push_node(&c->recv_wait, &nodes[i]);
            pthread_spin_unlock(&c->lock);
        }

        for (int i = 0; i < nsend; i++)
        {
            int ni = nrecv + i;
            ui_Chan *c = (ui_Chan *)(uintptr_t)send_chs[i];
            pthread_spin_lock(&c->lock);
            if (ui_select_try_send(c, send_vals[i]))
            {
                pthread_spin_unlock(&c->lock);
                ui_select_cleanup(recv_chs, send_chs, nodes, nrecv, nsend);
                free(nodes);
                return ni;
            }
            ui_waitq_push_node(&c->send_wait, &nodes[ni]);
            pthread_spin_unlock(&c->lock);
        }

        g->state = UI_WAITING;
        if (timeout_ms > 0)
        {
            uint64_t now = ui_now_us();
            if (now >= deadline ||
                ui_sleepq_push(v, g, deadline) != 0)
            {
                g->state = UI_READY;
                ui_select_cleanup(recv_chs, send_chs, nodes, nrecv, nsend);
                free(nodes);
                if (now >= deadline) return -1;
                ui_Yield();
                continue;
            }
        }

        ui_switch(&g->rsp, v->sched_rsp);

        ui_select_cleanup(recv_chs, send_chs, nodes, nrecv, nsend);
        free(nodes);

        if (timeout_ms > 0 && atomic_load_explicit(&ctx.fired, memory_order_acquire) == 0 &&
            ui_now_us() >= deadline)
            return -1;
    }
}
