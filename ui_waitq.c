#include "ui_internal.h"

void
ui_waitq_init(ui_WaitQ *q)
{
    q->head = NULL;
    q->count = 0;
}

int
ui_waitq_empty(ui_WaitQ *q)
{
    return q->head == NULL;
}

void
ui_waitq_push(ui_WaitQ *q, ui_Goro *g)
{
    g->wq_next = NULL;
    g->wq_prev = NULL;

    if (!q->head)
    {
        q->head = g;
        g->wq_next = g;
        g->wq_prev = g;
    }
    else
    {
        ui_Goro *last = q->head->wq_prev;
        last->wq_next = g;
        g->wq_prev = last;
        g->wq_next = q->head;
        q->head->wq_prev = g;
    }
    q->count++;
}

void
ui_waitq_remove(ui_WaitQ *q, ui_Goro *g)
{
    if (q->count == 0) return;

    if (g->wq_next == g)
    {
        q->head = NULL;
    }
    else
    {
        g->wq_prev->wq_next = g->wq_next;
        g->wq_next->wq_prev = g->wq_prev;
        if (q->head == g)
            q->head = g->wq_next;
    }
    g->wq_next = NULL;
    g->wq_prev = NULL;
    q->count--;
}

ui_Goro *
ui_waitq_pop(ui_WaitQ *q)
{
    if (!q->head) return NULL;
    ui_Goro *g = q->head;
    ui_waitq_remove(q, g);
    return g;
}

ui_Goro *
ui_waitq_peek(ui_WaitQ *q)
{
    return q->head;
}

void
ui_waitq_wake_one(ui_WaitQ *q)
{
    ui_Goro *g = ui_waitq_pop(q);
    if (g) ui_wakeup(g);
}

void
ui_waitq_wake_all(ui_WaitQ *q)
{
    while (q->head)
    {
        ui_Goro *g = ui_waitq_pop(q);
        if (g) ui_wakeup(g);
    }
}

int
ui_waitq_count(ui_WaitQ *q)
{
    return q->count;
}
