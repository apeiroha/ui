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
    ui_WaitNode *n = &g->wait_node;
    n->g = g;
    n->wake = NULL;
    n->data = NULL;
    ui_waitq_push_node(q, n);
}

void
ui_waitq_push_node(ui_WaitQ *q, ui_WaitNode *n)
{
    if (!q || !n || n->active)
        return;

    n->next = NULL;
    n->prev = NULL;

    if (!q->head)
    {
        q->head = n;
        n->next = n;
        n->prev = n;
    }
    else
    {
        ui_WaitNode *last = q->head->prev;
        last->next = n;
        n->prev = last;
        n->next = q->head;
        q->head->prev = n;
    }
    n->active = 1;
    q->count++;
}

void
ui_waitq_remove_node(ui_WaitQ *q, ui_WaitNode *n)
{
    if (!q || !n || !n->active || q->count == 0 || !q->head)
        return;

    /* Safety: verify node is actually in this queue */
    if (n->next == NULL && n->prev == NULL && q->head != n)
        return;  /* Node not in this queue */

    if (n->next == n)
    {
        q->head = NULL;
    }
    else
    {
        if (!n->prev || !n->next)
            return;  /* Corrupted node */
        
        n->prev->next = n->next;
        n->next->prev = n->prev;
        if (q->head == n)
            q->head = n->next;
    }
    n->next = NULL;
    n->prev = NULL;
    n->active = 0;
    q->count--;
}

static ui_WaitNode *
ui_waitq_pop_node(ui_WaitQ *q)
{
    if (!q->head) return NULL;
    ui_WaitNode *n = q->head;
    ui_waitq_remove_node(q, n);
    return n;
}

ui_Goro *
ui_waitq_pop(ui_WaitQ *q)
{
    ui_WaitNode *n = ui_waitq_pop_node(q);
    return n ? n->g : NULL;
}

ui_Goro *
ui_waitq_peek(ui_WaitQ *q)
{
    return q->head ? q->head->g : NULL;
}

static void
ui_waitq_wake_node(ui_WaitNode *n)
{
    if (!n || !n->g) return;
    if (n->wake)
        n->wake(n);
    else
        ui_wakeup(n->g);
}

void
ui_waitq_wake_one(ui_WaitQ *q)
{
    ui_WaitNode *n = ui_waitq_pop_node(q);
    ui_waitq_wake_node(n);
}

void
ui_waitq_wake_all(ui_WaitQ *q)
{
    while (q->head)
    {
        ui_WaitNode *n = ui_waitq_pop_node(q);
        ui_waitq_wake_node(n);
    }
}

int
ui_waitq_count(ui_WaitQ *q)
{
    return q->count;
}
