#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define UI_PAGE_SIZE        4096
#define UI_STACK_RESERVE    (8 * 1024 * 1024)
#define UI_STACK_INIT       (128 * 1024)
#define UI_RUNQ_CAP         256
#define UI_MAX_VCPUS        64

enum
{
    UI_READY,
    UI_RUNNING,
    UI_WAITING,
    UI_DEAD,
};

typedef struct ui_Goro ui_Goro;

struct ui_Goro
{
    void     *rsp;
    void     *stack_base;
    size_t    stack_reserve;
    size_t    stack_committed;
    int       page_size;

    ui_Func0  entry;
    void     *arg;

    int       state;
    int       home_vcpu;
    int       first_run;
    int       sleepq_idx;  /* index in sleepq heap, -1 if not in sleepq */
    /* Intrusive circular linked list (runq) */
    ui_Goro  *prev;
    ui_Goro  *next;
    /* Wait queue link */
    ui_Goro  *wq_prev;
    ui_Goro  *wq_next;

    ui_Goro  *joiner;

    uint64_t  wakeup_time;  /* absolute ms (monotonic), for sleepq */

    void     *chan_ptr;
    uint64_t  io_token;
    ssize_t   io_result;
};

typedef struct
{
    pthread_t        thread;
    int              id;
    /* Sentinel for intrusive circular runq list */
    ui_Goro          runq_sentinel;
    pthread_spinlock_t runq_lock;
    /* Goroutine pool (reuse stacks, avoid mmap) */
    ui_Goro         *goro_pool[16];
    int              goro_pool_count;
    int              event_fd;
    int              ring_fd;
    unsigned        *sq_head, *sq_tail, *sq_ring_mask, *sq_ring_entries;
    unsigned        *sq_flags, *sq_array;
    struct io_uring_sqe *sq_sqes;
    unsigned        *cq_head, *cq_tail, *cq_ring_mask, *cq_ring_entries;
    struct io_uring_cqe *cq_cqes;
    void            *sched_rsp;
    ui_Goro         *current;
    /* Sleep queue (binary min-heap by wakeup_time) */
    ui_Goro         *sleepq[256];
    int              sleepq_size;
    atomic_int       running;
    long             tick;
} ui_vCPU;

typedef struct
{
    ui_vCPU         *vcpus;
    int              nvcpus;
    atomic_int       active_count;
    pthread_mutex_t  global_lock;
    ui_Goro         *free_list;
    pthread_mutex_t  free_lock;

    struct sigaction old_sigsegv;
    stack_t          old_altstack;
    int              initialized;
} ui_Sched;

extern ui_Sched g_ui_sched;

extern __thread ui_Goro *ui_current_goro;

extern void   ui_switch(void **from_rsp, void *to_rsp);
extern void   ui_switch_defer(void **from_rsp, void *to_rsp,
                              void (*defer_fn)(void*), void *defer_arg);
extern void   ui_first_switch(void **sched_rsp_ptr, void *to_rsp);
extern void   ui_trampoline(void);

void          ui_schedule(void);
void          ui_wakeup(ui_Goro *g);
void          ui_goro_exit(void);

int           ui_stack_init(ui_Goro *g, int stack_size);
void          ui_stack_destroy(ui_Goro *g);
int           ui_stack_grow(ui_Goro *g, void *fault_addr);
void         *ui_stack_bottom(ui_Goro *g);

void          ui_runq_init(ui_vCPU *v);
void          ui_runq_insert(ui_vCPU *v, ui_Goro *g);
void          ui_runq_remove(ui_Goro *g);
int           ui_runq_empty(ui_vCPU *v);

void          ui_sleepq_push(ui_vCPU *v, ui_Goro *g, uint64_t deadline_ms);
void          ui_sleepq_remove(ui_vCPU *v, ui_Goro *g);
ui_Goro      *ui_sleepq_pop(ui_vCPU *v);
int           ui_sleepq_expire(ui_vCPU *v, uint64_t now_ms);
uint64_t      ui_now_ms(void);

/* ── Wait queue abstraction ── */

typedef struct {
    ui_Goro *head;
    int      count;
} ui_WaitQ;

void          ui_waitq_init(ui_WaitQ *q);
int           ui_waitq_empty(ui_WaitQ *q);
void          ui_waitq_push(ui_WaitQ *q, ui_Goro *g);
void          ui_waitq_remove(ui_WaitQ *q, ui_Goro *g);
ui_Goro      *ui_waitq_pop(ui_WaitQ *q);
ui_Goro      *ui_waitq_peek(ui_WaitQ *q);
void          ui_waitq_wake_one(ui_WaitQ *q);
void          ui_waitq_wake_all(ui_WaitQ *q);
int           ui_waitq_count(ui_WaitQ *q);

#endif
