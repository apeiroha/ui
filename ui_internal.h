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
    ui_Goro  *runq_next;
    ui_Goro  *wait_next;
    ui_Goro  *joiner;

    void     *chan_ptr;
    uint64_t  io_token;
    ssize_t   io_result;
};

typedef struct
{
    pthread_t        thread;
    int              id;
    atomic_int       runq_head;
    atomic_int       runq_tail;
    ui_Goro         *runq[UI_RUNQ_CAP];
    int              event_fd;
    int              ring_fd;
    unsigned        *sq_head, *sq_tail, *sq_ring_mask, *sq_ring_entries;
    unsigned        *sq_flags, *sq_array;
    struct io_uring_sqe *sq_sqes;
    unsigned        *cq_head, *cq_tail, *cq_ring_mask, *cq_ring_entries;
    struct io_uring_cqe *cq_cqes;
    void            *sched_rsp;
    ui_Goro         *current;
    atomic_int       running;
    long             tick;
} ui_vCPU;

typedef struct
{
    ui_vCPU         *vcpus;
    int              nvcpus;
    atomic_int       active_count;
    ui_Goro         *global_head;
    ui_Goro         *global_tail;
    pthread_mutex_t  global_lock;
    ui_Goro         *free_list;
    pthread_mutex_t  free_lock;

    struct sigaction old_sigsegv;
    stack_t          old_altstack;
    int              initialized;
} ui_Sched;

extern ui_Sched g_ui_sched;

extern void   ui_switch(void **from_rsp, void *to_rsp);
extern void   ui_trampoline(void);

void          ui_schedule(void);
void          ui_wakeup(ui_Goro *g);
void          ui_goro_exit(void);

int           ui_stack_init(ui_Goro *g, int stack_size);
void          ui_stack_destroy(ui_Goro *g);
int           ui_stack_grow(ui_Goro *g, void *fault_addr);
void         *ui_stack_bottom(ui_Goro *g);

#endif
