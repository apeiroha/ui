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
#include <linux/io_uring.h>

/* ── io_uring ABI fallbacks for older kernel headers ── */

#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT          (1U << 1)
#endif

#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER            (1U << 0)
#define IORING_CQE_BUFFER_SHIFT        16
#endif

#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE              (1U << 1)
#endif

/* TASKRUN_FLAG (Linux 5.19+): the kernel raises IORING_SQ_TASKRUN in the
 * SQ ring flags when it has deferred task work to run. */
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG      (1U << 9)
#endif
#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN               (1U << 2)
#endif

#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING      22
#define IORING_UNREGISTER_PBUF_RING    23
#endif

#ifndef IOU_PBUF_RING_MMAP
#define IOU_PBUF_RING_MMAP             1
#endif

/* ── RecvMulti struct ── */

struct ui_RecvMulti
{
    int                     fd;
    int                     active;
    int                     retired;       /* Close called; awaiting final CQEs */
    int                     cancel_seen;   /* ASYNC_CANCEL completion reaped */
    int                     terminal_seen; /* multishot terminal CQE reaped */
    ui_RecvMultiCb          cb;
    void                   *ctx;
    struct msghdr           msg;
    struct iovec            iov;
    struct sockaddr_storage addr;
    uint32_t                msg_namelen;    /* reserved name buf size */
    uint32_t                msg_controllen; /* reserved ctrl buf size */
    int                     vcpu;           /* ring/list owner at creation */
    struct ui_RecvMulti    *next;
    struct ui_RecvMulti    *close_next;     /* owner close queue */
};

#define UI_PAGE_SIZE        4096
#define UI_STACK_RESERVE    (8 * 1024 * 1024)
#define UI_STACK_INIT       (128 * 1024)
#define UI_RUNQ_CAP         256
#define UI_MAX_VCPUS        64
#define UI_GORO_PREALLOC    256    /* pre-alloc goros + stacks per process */
#define UI_GORO_POOL_SIZE   16384  /* max cached goros + stacks per process */
#define UI_LOCAL_GORO_POOL_SIZE 2048

/* I/O fairness: after this many consecutive fast-path I/O completions,
 * the goro yields voluntarily to let others run.  0 = disabled.
 * Can be overridden via env UI_YIELD_IO_MASK at ui_Init() time. */
#define UI_YIELD_IO_MASK_DEFAULT  255   /* yield every 256th I/O */
extern uint32_t ui_yield_io_mask;

/* Stack trim policy: what happens to a goroutine's stack pages when the
 * goroutine goes back to the pool.  The policy itself (and the public
 * API around it) lives in ui.h; this is only the internal state.
 *
 * Overridable via env UI_STACK_TRIM (0/1/2) at ui_Init() time, or at
 * runtime through ui_SetStackTrimPolicy(). */
/* Keep the pages by default: a steady-state goroutine spawn costs 298ns
 * with them kept, 1994ns under MADV_FREE and 3022ns under
 * MADV_DONTNEED (400k spawns, median of 3).  MADV_FREE does hold ~5MB
 * less (18.4 vs 23.8MB), but a deployment that wants a low resident set
 * does better with ui_SetStackMemoryLimit(): NONE plus a 4MB budget is
 * 866ns at 5.7MB, both faster and leaner than MADV_FREE.  Keep the
 * default fast; let the budget handle memory. */
#define UI_STACK_TRIM_DEFAULT  UI_STACK_TRIM_NONE
extern int ui_stack_trim_policy;

/* Cap on the stack memory the goroutine pool may keep (bytes).  0 = no
 * cap.  A non-zero cap also switches the trim policy to the strict
 * one (MADV_DONTNEED) and makes the pool unmap stacks once the budget
 * is exhausted.  Overridable via env UI_STACK_MEMORY_LIMIT. */
extern unsigned long long ui_stack_memory_limit;
/* Estimated resident bytes held by the pooled goroutine stacks. */
extern unsigned long long ui_stack_pool_bytes;

/* How many recycles happen between two hard trims (MADV_DONTNEED) of the
 * per-vCPU pool.  HARD trims are what keep the resident set of pooled
 * stacks bounded when the default MADV_FREE policy is used; sweeping
 * every N recycles amortises their cost to ~1/N of a MADV_FREE call. */
#define UI_STACK_TRIM_SWEEP_INTERVAL  1024

/* Upper bound on how long a parked vCPU blocks before re-checking for
 * work it was never notified about.  Every real wake source (cross-vCPU
 * wakeup, I/O completion, sleepq deadline) signals directly, so this is
 * a lost-wakeup watchdog, not a polling interval — and it dominates
 * idle CPU, since each expiry costs one syscall per vCPU.
 * Overridable via env UI_IDLE_WAIT_MS at ui_Init() time. */
#define UI_IDLE_WAIT_MS_DEFAULT   100   /* ms */
extern uint32_t ui_idle_wait_ms;

/* Idle spin iterations before blocking (roughly 0.5us of pause loops).
 * Cuts the eventfd+ppoll round trip for wakes that arrive while the
 * vCPU is still on-CPU looking for work. */
#define UI_IDLE_SPIN_ITERS        64
/* LIFO slot: consecutive schedules served from runnext before the
 * occupant is demoted to the FIFO tail once (Tokio PR #2349 analog —
 * ui has no preemption, so the cap is the only anti-monopoly guard). */
#define UI_RUNNEXT_MAX_POLLS      3
/* Pause-spin iterations approximating Go's usleep(3) backoff before
 * stealing a running vCPU's runnext (~50ns chan op x 50 overshoot). */
#define UI_RUNNEXT_BACKOFF_ITERS  3000

enum
{
    UI_READY,
    UI_RUNNING,
    UI_WAITING,
    UI_DEAD,
};

typedef struct ui_Goro ui_Goro;
typedef struct ui_WaitNode ui_WaitNode;
typedef struct ui_StackArena ui_StackArena;

struct ui_WaitNode
{
    ui_Goro     *g;
    ui_WaitNode *prev;
    ui_WaitNode *next;
    void       (*wake)(ui_WaitNode *n);
    void        *data;
    int          active;
};

struct ui_Goro
{
    void     *rsp;
    void     *stack_base;
    size_t    stack_reserve;
    size_t    stack_committed;
    ui_StackArena *stack_arena;
    int       stack_slot;
    int       page_size;
    /* Lowest stack address this goroutine has been seen at (absolute),
     * sampled when it switches out.  0 = unknown.
     *
     * This is only an ESTIMATE, and a low one: a goroutine that recurses
     * and then returns has its frames gone by the time it switches out,
     * so the mark stays near the top.  It is used for the stack memory
     * budget (a soft cap, deliberately lenient) and NOT for the trim
     * range, which always covers the whole committed region. */
    uintptr_t stack_low;

    ui_Func0  entry;
    void     *arg;

    int       state;
    int       voluntary;   /* 1 = last yield was a voluntary ui_Yield */
    int       home_vcpu;
    int       first_run;
    int       pinned;      /* 1 = GoOn-placed: not stealable until it has
                              * run once on its requested vCPU.  Also keeps
                              * a bound goro out of foreign LIFO slots and
                              * stops channel handoff from rewriting its
                              * home.  GoOn sets and first-schedule clears
                              * it. */
    int       bound;       /* 1 = ui_PinTo'd: a user-set home that channel
                              * direct handoff must not rewrite.  Unlike
                              * `pinned` this grants NO steal immunity --
                              * ui_PinTo is documented as a soft binding
                              * that work stealing may still migrate.
                              * Cleared once the goro resumes on that home
                              * (ui_schedule), so it only covers the window
                              * between the call and the first wakeup there. */
#ifdef UI_DEBUG_LIFO
    /* TEMP DEBUG (lifo investigation) — build with -DUI_DEBUG_LIFO */
    int       dbg_slot_writer;
    int       dbg_state_at_fill;
    int       dbg_prev_voluntary;
    int       dbg_d0v, dbg_d0k;
    int       dbg_d1v, dbg_d1k;
#endif
    int       sleepq_idx;  /* index in sleepq heap, -1 if not in sleepq */
    atomic_int sleepq_vcpu;/* owning vCPU's sleepq, -1 if not queued */
    /* Intrusive circular linked list (runq) */
    ui_Goro  *prev;
    ui_Goro  *next;
    ui_WaitNode wait_node;
    ui_Goro  *free_next;
    ui_Goro  *standby_next;

    uint64_t  wakeup_time;  /* absolute us (monotonic), for sleepq */

    int       rwlock_shard;  /* shard used by the currently held read lock */

    void     *chan_ptr;
    void     *chan_recv_ptr;  /* dest buffer for recv handoff */
    const void *chan_send_ptr;/* src data for send handoff */
    int       chan_handoff;   /* 1 = direct handoff completed */
    uint64_t  io_token;  /* 代际令牌：每次 I/O 提交自增，跨 goro 复用不清零；
                              同一地址的第 N 世与第 N+1 世 I/O 世代可区分 */
    ssize_t   io_result;
    int       io_pending;  /* non-zero while an io_uring op is in flight */
};

typedef struct
{
    /* Sentinel for intrusive circular runq list */
    ui_Goro          runq_sentinel;
    pthread_t        thread;
    int              id;
    pthread_spinlock_t runq_lock;
    atomic_int       runq_count;
    /* Per-vCPU goro pool (only accessed by this vCPU = no lock) */
    ui_Goro         *goro_pool[UI_LOCAL_GORO_POOL_SIZE];
    int              goro_pool_count;
    atomic_int       idle;
    int              event_fd;
    int              ring_fd;
    unsigned        *sq_head, *sq_tail, *sq_ring_mask, *sq_ring_entries;
    unsigned        *sq_flags, *sq_array;
    struct io_uring_sqe *sq_sqes;
    unsigned        *cq_head, *cq_tail, *cq_ring_mask, *cq_ring_entries;
    struct io_uring_cqe *cq_cqes;
    void            *sq_ring_ptr;
    void            *cq_ring_ptr;
    size_t           sq_ring_size;
    size_t           cq_ring_size;
    size_t           sqes_size;
    int              no_sq_array;  /* IORING_SETUP_NO_SQARRAY was enabled */
    int              taskrun_flag; /* IORING_SETUP_TASKRUN_FLAG was accepted:
                                    * the kernel raises IORING_SQ_TASKRUN when
                                    * it has deferred task work to run */
    int              uring_pending;
    /* Multishot recvmsg + buffer ring */
    int              bgid;                 /* buf group id, -1 = unregistered */
    int              buf_ring_count;
    int              buf_ring_buf_size;
    struct io_uring_buf_ring *buf_ring;    /* mmap'd buf ring metadata */
    void            *buf_ring_bufs;        /* mmap'd buffer data */
    size_t           buf_ring_mmap_sz;
    int              has_multishot;
    /* Ring posts every completion to event_fd (IORING_REGISTER_EVENTFD).
     * This is what lets a single ppoll(event_fd) cover I/O completions
     * as well as cross-vCPU wakeups; without it only a poll timeout can
     * observe an I/O completion.  0 if registration failed. */
    int              cq_eventfd;
    struct ui_RecvMulti *active_multishot;
    /* Closed multishots awaiting their cancel+terminal CQEs before free. */
    struct ui_RecvMulti *retired_multishot;
    /* Cross-vCPU closes are deferred to the owner (single issuer). */
    struct ui_RecvMulti *rm_closeq_head;
    struct ui_RecvMulti *rm_closeq_tail;
    pthread_spinlock_t rm_closeq_lock;
    atomic_int       rm_closeq_len;
    void            *sched_rsp;
    ui_Goro         *current;
    /* Sleep queue (binary min-heap by wakeup_time) */
    ui_Goro         *sleepq[256];
    int              sleepq_size;
    pthread_spinlock_t sleepq_lock;
    atomic_int       sleepq_len;      /* mirror of sleepq_size for fast checks */
    atomic_uint_least64_t sleepq_top_us; /* earliest deadline, UINT64_MAX=empty */
    atomic_int       running;
    long             tick;
    uint32_t         rng_state;
    /* Standbyq — lock-protected list for cross-vCPU migration.
     * Only the owning vCPU reads/drains; any vCPU may push. */
    ui_Goro         *standbyq_head;
    ui_Goro         *standbyq_tail;
    pthread_spinlock_t standbyq_lock;
    uint32_t         io_count;   /* sequential I/O completions, for fair yield */
    /* LIFO slot (Go runnext / Tokio lifo_slot): the most recently
     * readied goro on this vCPU.  Protected by runq_lock.  Consumed
     * before the FIFO runq; stealable only as a last resort with a
     * short backoff while the victim is actively running (mirrors
     * Go's 3us usleep before stealing a running P's runnext). */
    ui_Goro         *runnext;
    int              runnext_polls;   /* consecutive schedules from slot */
    /* Recycles since the last hard trim of this vCPU's pool, and where
     * in the pool that sweep should start (the window rotates so the
     * whole pool gets covered - see ui_goro_pool_sweep). */
    int              trim_sweep_count;
    int              trim_sweep_at;
    /* Cache-line aligned: the vCPUs live in one array, so without this
     * the last fields of one vCPU share a cache line with the first
     * fields of the next one.  `runnext` in particular is written by
     * the owner and read by every thief. */
} __attribute__((aligned(64))) ui_vCPU;

_Static_assert(sizeof(ui_vCPU) % 64 == 0,
               "ui_vCPU must be a multiple of the cache line size");

typedef struct
{
    ui_vCPU         *vcpus;
    int              nvcpus;
    atomic_int       started;
    atomic_int       active_count;
    atomic_int       next_vcpu;
    /* Pool of goros with stacks preserved (never munmap'd).
     * Mutex-protected array; contention is negligible vs mmap savings. */
    ui_Goro         *goro_pool[UI_GORO_POOL_SIZE];
    int              goro_pool_count;
    pthread_mutex_t  goro_pool_lock;
    ui_StackArena   *stack_arenas;
    pthread_mutex_t  stack_arena_lock;

    struct sigaction old_sigsegv;
    stack_t          old_altstack;
    int              initialized;
    int              finalizing;
} ui_Sched;

extern ui_Sched g_ui_sched;

extern __thread __attribute__((tls_model("initial-exec"))) ui_Goro *ui_current_goro;
extern __thread __attribute__((tls_model("initial-exec"))) ui_vCPU *ui_this_vcpu;

extern void   ui_switch(void **from_rsp, void *to_rsp);
extern void   ui_switch_defer(void **from_rsp, void *to_rsp,
                              void (*defer_fn)(void*), void *defer_arg);
extern void   ui_first_switch(void **sched_rsp_ptr, void *to_rsp);
extern void   ui_trampoline(void);

void          ui_schedule(void);
void          ui_vcpu_idle(ui_vCPU *v);
void          ui_wakeup(ui_Goro *g);
void          ui_wakeup_handoff(ui_Goro *g);
void          ui_wake_vcpu(ui_vCPU *v);
void          ui_goro_exit(void);

int           ui_stack_init(ui_Goro *g, int stack_size);
void          ui_stack_destroy(ui_Goro *g);
void          ui_stack_trim(ui_Goro *g);          /* policy-driven (see ui_StackTrimPolicy) */
void          ui_stack_hard_trim(ui_Goro *g);     /* MADV_DONTNEED, ignores the policy */

#ifdef UI_DEBUG_STACK_TRIM
/* Test hook: what the trim path asked the kernel for.  ui_debug_trim_calls
 * counts the madvise() calls, ui_debug_trim_advice is the advice of the
 * last one (MADV_FREE / MADV_DONTNEED). */
extern int    ui_debug_trim_calls;
extern int    ui_debug_trim_advice;
extern unsigned long ui_debug_trim_start, ui_debug_trim_len;
extern unsigned long ui_debug_stack_base, ui_debug_stack_committed, ui_debug_stack_low;
#endif
void          ui_stack_arenas_destroy(void);
int           ui_stack_grow(ui_Goro *g, void *fault_addr);
void         *ui_stack_bottom(ui_Goro *g);

void          ui_runq_init(ui_vCPU *v);
void          ui_runq_insert(ui_vCPU *v, ui_Goro *g);
void          ui_runq_insert_locked(ui_vCPU *v, ui_Goro *g); /* caller holds runq_lock */
void          ui_runq_remove(ui_Goro *g);
int           ui_runq_empty(ui_vCPU *v);

void          ui_standbyq_init(ui_vCPU *v);
void          ui_standbyq_push(ui_vCPU *v, ui_Goro *g);

int           ui_sleepq_push(ui_vCPU *v, ui_Goro *g, uint64_t deadline_us);
void          ui_sleepq_remove(ui_Goro *g);
ui_Goro      *ui_sleepq_pop(ui_vCPU *v);
int           ui_sleepq_expire(ui_vCPU *v, uint64_t now_us);
uint64_t      ui_now_ms(void);
uint64_t      ui_now_us(void);

int           ui_vcpu_ensure_ring(ui_vCPU *v);
int           ui_vcpu_ensure_buf_ring(ui_vCPU *v);
void          ui_vcpu_destroy_buf_ring(ui_vCPU *v);
void          ui_uring_drain(ui_vCPU *v);
int           ui_uring_taskrun_pending(ui_vCPU *v);
void          ui_recvmulti_process_closeq(ui_vCPU *v);
void          ui_uring_idle_wait(ui_vCPU *v, uint64_t wait_us);
int           ui_uring_enter(int ring_fd, unsigned to_submit,
                             unsigned min_complete, unsigned flags);

/* ── Wait queue abstraction ── */

typedef struct {
    ui_WaitNode *head;
    int          count;
} ui_WaitQ;

void          ui_waitq_init(ui_WaitQ *q);
int           ui_waitq_empty(ui_WaitQ *q);
void          ui_waitq_push(ui_WaitQ *q, ui_Goro *g);
void          ui_waitq_push_node(ui_WaitQ *q, ui_WaitNode *n);
void          ui_waitq_remove_node(ui_WaitQ *q, ui_WaitNode *n);
ui_Goro      *ui_waitq_pop(ui_WaitQ *q);
ui_Goro      *ui_waitq_peek(ui_WaitQ *q);
void          ui_waitq_wake_one(ui_WaitQ *q);
void          ui_waitq_wake_all(ui_WaitQ *q);
int           ui_waitq_count(ui_WaitQ *q);

/* ── Batch recv — 堆分配；drain 收割完该批全部 CQE 后释放。
 * 不能放调用者栈上：残留 CQE（数据或 ECANCELED）可能在函数返回后
 * 才到达，届时栈帧已死，drain 解引用 &batch 即 use-after-scope。 ── */
struct ui_RecvBatch {
    ui_Goro  *goro;
    int       count;   /* protected by lock */
    int       active;  /* 1 while caller is still waiting; protected by lock */
    unsigned  total;   /* read-only after submit */
    pthread_spinlock_t lock;
    int       refs;    /* caller + ring; last one frees */
};

#endif
