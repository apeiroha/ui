#include "ui_internal.h"
#ifdef UI_DEBUG_LIFO
#include <ucontext.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

ui_Sched g_ui_sched;

__thread __attribute__((tls_model("initial-exec"))) ui_vCPU *ui_this_vcpu;

/* I/O fairness yield mask: yield every (mask+1)th fast-path I/O.
 * 0 = disabled.  Override via env UI_YIELD_IO_MASK. */
uint32_t ui_yield_io_mask = UI_YIELD_IO_MASK_DEFAULT;

/* Lost-wakeup watchdog for the idle block.  Override via env
 * UI_IDLE_WAIT_MS; idle CPU is dominated by this value, since each
 * expiry costs one syscall per parked vCPU. */
uint32_t ui_idle_wait_ms = UI_IDLE_WAIT_MS_DEFAULT;

/* Stack trim policy for recycled goroutines.  Override via env
 * UI_STACK_TRIM (0=none, 1=MADV_FREE, 2=MADV_DONTNEED) or at runtime
 * through ui_SetStackTrimPolicy(). */
int ui_stack_trim_policy = UI_STACK_TRIM_DEFAULT;

/* Cap on the stack memory the goroutine pool may keep.  Override via
 * env UI_STACK_MEMORY_LIMIT or ui_SetStackMemoryLimit(). */
unsigned long long ui_stack_memory_limit;

/* Estimated resident bytes held by the pooled goroutine stacks. */
unsigned long long ui_stack_pool_bytes;

/* Values set through the public API take precedence over the
 * environment.  -1 / 0 mean "not set through the API". */
static int ui_stack_trim_policy_api = -1;
static unsigned long long ui_stack_memory_limit_api;
static int ui_stack_memory_limit_api_set;

/* Estimated resident bytes of a goroutine's stack: the pages between
 * the stack top and the deepest address the goroutine was ever seen at.
 *
 * An underestimate - see stack_low.  That is acceptable because the
 * budget is a soft cap whose job is to switch the pool to "memory
 * first" (MADV_DONTNEED plus dropping stacks), not to hit a byte
 * target exactly. */
static unsigned long long
ui_goro_stack_bytes(ui_Goro *g)
{
    if (!g->stack_base || !g->stack_low)
        return 0;
    unsigned long long top = (unsigned long long)g->stack_base + g->stack_reserve;
    unsigned long long low = (unsigned long long)g->stack_low;
    if (low >= top)
        return 0;
    unsigned long long ps = g->page_size > 0 ? (unsigned long long)g->page_size
                                             : (unsigned long long)UI_PAGE_SIZE;
    return ((top - low + ps - 1) / ps) * ps;
}

#ifdef UI_DEBUG_GORO_LIMIT
/* Test hook: see ui_goro_alloc().  0 = unlimited. */
int ui_debug_goro_limit;
static int ui_debug_goro_alloc_count;
#endif

void
ui_runq_init(ui_vCPU *v)
{
    v->runq_sentinel.next = &v->runq_sentinel;
    v->runq_sentinel.prev = &v->runq_sentinel;
    atomic_store(&v->runq_count, 0);
}

/* Insert into runq (caller must hold runq_lock) */
void
ui_runq_insert_locked(ui_vCPU *v, ui_Goro *g);

void
ui_runq_insert(ui_vCPU *v, ui_Goro *g)
{
    pthread_spin_lock(&v->runq_lock);
    ui_runq_insert_locked(v, g);
    pthread_spin_unlock(&v->runq_lock);
}

/* Insert into runq (caller must hold runq_lock) */
void
ui_runq_insert_locked(ui_vCPU *v, ui_Goro *g)
{
    if (g->state != UI_READY)
        return;
    if (g->prev != NULL)
    {
        return;
    }
    /* Assert: goros must have a valid stack pointer when inserted */
    if (!g->first_run && g->rsp == 0) {
        fprintf(stderr, "\nFATAL: runq_insert goro %p rsp=0 v=%d state=%d home=%d "
                "first=%d io_pend=%d\n",
                g, v->id, g->state, g->home_vcpu, g->first_run, g->io_pending);
        _exit(1);
    }
    ui_Goro *s = &v->runq_sentinel;
    ui_Goro *last = s->prev;
    g->next = s;
    g->prev = last;
    last->next = g;
    s->prev = g;
    atomic_fetch_add(&v->runq_count, 1);
}

#ifdef UI_DEBUG_LIFO
/* TEMP DEBUG: record a delivery of g by vcpu v with kind k */
static inline void ui_dbg_deliver(ui_Goro *g, int v, int k)
{
    if (!g) return;
    g->dbg_d1v = g->dbg_d0v; g->dbg_d1k = g->dbg_d0k;
    g->dbg_d0v = v;          g->dbg_d0k = k;
}

/* TEMP DEBUG: waking a goro that is currently RUNNING somewhere = bug */
static void ui_dbg_check_not_current(ui_Goro *g)
{
    if (!g) return;
    for (int vi = 0; vi < g_ui_sched.nvcpus; vi++) {
        ui_vCPU *vv = &g_ui_sched.vcpus[vi];
        if (vv->current == g) {
            fprintf(stderr,
                "[dbg-wakecur] waking g=%p but CURRENT on vcpu %d "
                "(waker vcpu %p)\n",
                (void *)g, vi, (void *)ui_this_vcpu);
            abort();
        }
    }
}
#else
#define ui_dbg_deliver(g, v, k)          ((void)0)
#define ui_dbg_check_not_current(g)      ((void)0)
#endif

void
ui_runq_remove(ui_Goro *g)
{
    ui_vCPU *v = &g_ui_sched.vcpus[g->home_vcpu];
    pthread_spin_lock(&v->runq_lock);
    if (!g->prev || !g->next)
    {
        pthread_spin_unlock(&v->runq_lock);
        return;
    }
    g->prev->next = g->next;
    g->next->prev = g->prev;
    g->next = NULL;
    g->prev = NULL;
    atomic_fetch_sub(&v->runq_count, 1);
    pthread_spin_unlock(&v->runq_lock);
}

int
ui_runq_empty(ui_vCPU *v)
{
    return atomic_load(&v->runq_count) == 0;
}

void
ui_wake_vcpu(ui_vCPU *v)
{
    if (!v || v->event_fd < 0)
        return;

    int expected = 1;
    if (atomic_compare_exchange_strong(&v->idle, &expected, 0))
    {
        uint64_t val = 1;
        write(v->event_fd, &val, sizeof(val));
    }
}

/* ── Standbyq (cross-vCPU migration queue) ── */

void
ui_standbyq_init(ui_vCPU *v)
{
    v->standbyq_head = NULL;
    v->standbyq_tail = NULL;
    pthread_spin_init(&v->standbyq_lock, PTHREAD_PROCESS_PRIVATE);
}

void
ui_standbyq_push(ui_vCPU *v, ui_Goro *g)
{
    g->standby_next = NULL;
    pthread_spin_lock(&v->standbyq_lock);
    if (v->standbyq_tail)
        v->standbyq_tail->standby_next = g;
    else
        v->standbyq_head = g;
    v->standbyq_tail = g;
    pthread_spin_unlock(&v->standbyq_lock);
}

/* Drain all goroutines from standbyq into the runq.
 * Called at the start of each ui_schedule(). */
static void
ui_drain_standbyq(ui_vCPU *v)
{
    pthread_spin_lock(&v->standbyq_lock);
    ui_Goro *head = v->standbyq_head;
    v->standbyq_head = NULL;
    v->standbyq_tail = NULL;
    pthread_spin_unlock(&v->standbyq_lock);

    while (head)
    {
        ui_Goro *next = head->standby_next;
        head->standby_next = NULL;
        if (head->state == UI_READY)
        {
            if (atomic_load_explicit(&head->sleepq_vcpu,
                                     memory_order_relaxed) >= 0)
                ui_sleepq_remove(head);
            ui_dbg_deliver(head, v->id, 1);
            ui_runq_insert(v, head);
        }
        head = next;
    }
}

/* ── Sleep queue (min-heap by wakeup_time) ── */

uint64_t
ui_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

uint64_t
ui_now_ms(void)
{
    return ui_now_us() / 1000;
}

static void
sleepq_sift_up(ui_vCPU *v, int idx)
{
    while (idx > 0)
    {
        int parent = (idx - 1) / 2;
        if (v->sleepq[idx]->wakeup_time >= v->sleepq[parent]->wakeup_time)
            break;
        ui_Goro *tmp = v->sleepq[idx];
        v->sleepq[idx] = v->sleepq[parent];
        v->sleepq[parent] = tmp;
        v->sleepq[idx]->sleepq_idx = idx;
        v->sleepq[parent]->sleepq_idx = parent;
        idx = parent;
    }
}

static void
sleepq_sift_down(ui_vCPU *v, int idx)
{
    int size = v->sleepq_size;
    for (;;)
    {
        int smallest = idx;
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        if (left < size && v->sleepq[left]->wakeup_time < v->sleepq[smallest]->wakeup_time)
            smallest = left;
        if (right < size && v->sleepq[right]->wakeup_time < v->sleepq[smallest]->wakeup_time)
            smallest = right;
        if (smallest == idx)
            break;
        ui_Goro *tmp = v->sleepq[idx];
        v->sleepq[idx] = v->sleepq[smallest];
        v->sleepq[smallest] = tmp;
        v->sleepq[idx]->sleepq_idx = idx;
        v->sleepq[smallest]->sleepq_idx = smallest;
        idx = smallest;
    }
}

static void
sleepq_meta_update(ui_vCPU *v)
{
    atomic_store_explicit(&v->sleepq_len, v->sleepq_size,
                          memory_order_relaxed);
    uint64_t top = v->sleepq_size > 0 ? v->sleepq[0]->wakeup_time
                                      : UINT64_MAX;
    atomic_store_explicit(&v->sleepq_top_us, top, memory_order_relaxed);
}

/* Caller holds v->sleepq_lock. */
static ui_Goro *
sleepq_pop_locked(ui_vCPU *v)
{
    if (v->sleepq_size == 0)
        return NULL;
    ui_Goro *g = v->sleepq[0];
    v->sleepq_size--;
    if (v->sleepq_size > 0)
    {
        v->sleepq[0] = v->sleepq[v->sleepq_size];
        v->sleepq[0]->sleepq_idx = 0;
        sleepq_sift_down(v, 0);
    }
    g->sleepq_idx = -1;
    atomic_store_explicit(&g->sleepq_vcpu, -1, memory_order_relaxed);
    return g;
}

int
ui_sleepq_push(ui_vCPU *v, ui_Goro *g, uint64_t deadline_us)
{
    pthread_spin_lock(&v->sleepq_lock);
    if (v->sleepq_size >= 256)
    {
        pthread_spin_unlock(&v->sleepq_lock);
        return -1;
    }
    g->wakeup_time = deadline_us;
    g->sleepq_idx = v->sleepq_size;
    atomic_store_explicit(&g->sleepq_vcpu, v->id, memory_order_release);
    v->sleepq[v->sleepq_size] = g;
    v->sleepq_size++;
    sleepq_sift_up(v, g->sleepq_idx);
    sleepq_meta_update(v);
    pthread_spin_unlock(&v->sleepq_lock);
    return 0;
}

void
ui_sleepq_remove(ui_Goro *g)
{
    int sv = atomic_load_explicit(&g->sleepq_vcpu, memory_order_acquire);
    if (sv < 0 || sv >= g_ui_sched.nvcpus)
        return;

    ui_vCPU *v = &g_ui_sched.vcpus[sv];
    pthread_spin_lock(&v->sleepq_lock);
    int idx = g->sleepq_idx;
    if (idx < 0 || idx >= v->sleepq_size || v->sleepq[idx] != g)
    {
        pthread_spin_unlock(&v->sleepq_lock);
        return;
    }
    v->sleepq_size--;
    if (idx < v->sleepq_size)
    {
        v->sleepq[idx] = v->sleepq[v->sleepq_size];
        v->sleepq[idx]->sleepq_idx = idx;
        sleepq_sift_down(v, idx);
        sleepq_sift_up(v, idx);
    }
    g->sleepq_idx = -1;
    atomic_store_explicit(&g->sleepq_vcpu, -1, memory_order_relaxed);
    sleepq_meta_update(v);
    pthread_spin_unlock(&v->sleepq_lock);
}

ui_Goro *
ui_sleepq_pop(ui_vCPU *v)
{
    pthread_spin_lock(&v->sleepq_lock);
    ui_Goro *g = sleepq_pop_locked(v);
    sleepq_meta_update(v);
    pthread_spin_unlock(&v->sleepq_lock);
    return g;
}

int
ui_sleepq_expire(ui_vCPU *v, uint64_t now_us)
{
    ui_Goro *ready[256];
    int n = 0, count = 0;

    pthread_spin_lock(&v->sleepq_lock);
    while (v->sleepq_size > 0 && v->sleepq[0]->wakeup_time <= now_us)
    {
        ready[n++] = sleepq_pop_locked(v);
        if (n == 256)
            break;
    }
    sleepq_meta_update(v);
    pthread_spin_unlock(&v->sleepq_lock);

    /* A concurrent waker may have already readied the goro and queued it
     * on its home vCPU; only enqueue if we win the WAITING->READY race. */
    for (int i = 0; i < n; i++)
    {
        ui_Goro *g = ready[i];
        if (__sync_bool_compare_and_swap(&g->state, UI_WAITING, UI_READY))
        {
            ui_dbg_deliver(g, v->id, 2);
            ui_runq_insert(v, g);
            count++;
        }
    }
    return count;
}

/* ── Per-vCPU xorshift32 (replaces non-thread-safe rand()) ── */

static uint32_t
ui_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ── Work stealing ── */

static int
ui_steal_work(ui_vCPU *v)
{
    if (g_ui_sched.nvcpus <= 1)
        return 0;

    for (int attempt = 0; attempt < g_ui_sched.nvcpus * 2; attempt++)
    {
        int vid = (int)(ui_xorshift32(&v->rng_state) % (uint32_t)g_ui_sched.nvcpus);
        if (vid == v->id) continue;

        ui_vCPU *vic = &g_ui_sched.vcpus[vid];
        if (pthread_spin_trylock(&vic->runq_lock) != 0) continue;
        if (pthread_spin_trylock(&v->runq_lock) != 0)
        {
            pthread_spin_unlock(&vic->runq_lock);
            continue;
        }

        if (!ui_runq_empty(v))
        {
            pthread_spin_unlock(&v->runq_lock);
            pthread_spin_unlock(&vic->runq_lock);
            return 0;
        }

        ui_Goro *s = &vic->runq_sentinel;
        int count = atomic_load(&vic->runq_count);

        bool ok = false;
        /* Stealability must hold for ANY nonzero runq depth: a lone spawned
         * goro behind a busy spawner would otherwise never migrate (the
         * spawner holds its vCPU until it blocks — no preemption) and idle
         * peers would spin forever without picking it up.  Go's runqgrab
         * takes n - n/2 (a single g IS stealable); Tokio hit the identical
         * pathology with a non-stealable slot and fixed it by making it
         * stealable (#4941). */
        if (count >= 1)
        {
            int want = count > 1 ? count / 2 : 1;
            ui_Goro *batch_end = s->prev;
            /* GoOn-pinned goros are exempt from stealing until they have
             * run once on their requested vCPU: the batch is trimmed to
             * the contiguous newest run of unpinned candidates, so a
             * pinned goro deeper in the queue shields itself and anything
             * older.  If even the newest candidate is pinned, skip. */
            if (!batch_end->pinned)
            {
                ui_Goro *batch_start = batch_end;
                int taken = 1;
                while (taken < want && batch_start->prev != s &&
                       !batch_start->prev->pinned)
                {
                    batch_start = batch_start->prev;
                    taken++;
                }

                batch_start->prev->next = s;
                s->prev = batch_start->prev;
                atomic_fetch_sub(&vic->runq_count, taken);

                ui_Goro *steal = batch_start;
                for (;;)
                {
                    ui_Goro *next = steal->next;
                    steal->next = NULL;
                    steal->prev = NULL;
                    steal->home_vcpu = v->id;
                    ui_dbg_deliver(steal, v->id, 3);
                    ui_runq_insert_locked(v, steal);
                    if (steal == batch_end) break;
                    steal = next;
                }
                ok = 1;
            }
        }

        pthread_spin_unlock(&v->runq_lock);
        if (ok)
        {
            pthread_spin_unlock(&vic->runq_lock);
            return 1;
        }

        /* Last resort: steal the victim's LIFO slot.  Mandatory for the
         * lone-spawn starvation fix — a freshly spawned goro now lands
         * in its spawner's slot, and with no preemption an idle peer
         * must be able to take it when the spawner never yields.  Like
         * Go (runqgrab stealRunNextG on the final findRunnable pass),
         * this is only attempted on the last attempt; and when the
         * victim is actively running a goro we pause-spin ~3us first —
         * sync chan ops take ~50ns, so the backoff gives the pair a
         * ~50x window to finish their handoff locally instead of being
         * thrashed across cores. */
        if (attempt != g_ui_sched.nvcpus * 2 - 1 || !vic->runnext ||
            vic->runnext->pinned)
        {
            pthread_spin_unlock(&vic->runq_lock);
            continue;
        }

        /* Backoff OUTSIDE both runq locks: spinning while holding the
         * victim's lock would stall exactly the handoff we are waiting
         * out.  State may change during the spin — everything is
         * revalidated after re-locking. */
        {
            ui_vCPU *vc = vic;
            /* The victim may clear ->current between the NULL check and
             * the state load.  Read the pointer once: re-reading it in the
             * second operand can dereference NULL (seen as a fault at 0x40
             * in the r11c stress). */
            ui_Goro *cur = vc->current;
            int cur_running = cur && cur->state == UI_RUNNING;
            pthread_spin_unlock(&vic->runq_lock);
            if (cur_running)
                for (int k = 0; k < UI_RUNNEXT_BACKOFF_ITERS; k++)
                    __builtin_ia32_pause();
        }

        /* Re-lock in vCPU-id order: two stealers can otherwise take each
         * other's runq_lock in victim->self order and deadlock (ABBA). */
        ui_vCPU *lo = v->id < vic->id ? v : vic;
        ui_vCPU *hi = lo == v ? vic : v;
        pthread_spin_lock(&lo->runq_lock);
        pthread_spin_lock(&hi->runq_lock);

        ui_Goro *rn = vic->runnext;
        if (!ui_runq_empty(v) || !rn || rn->pinned)
        {
            pthread_spin_unlock(&v->runq_lock);
            pthread_spin_unlock(&vic->runq_lock);
            if (!ui_runq_empty(v)) return 0;
            continue;
        }

        vic->runnext = NULL;
        rn->next = NULL;
        rn->prev = NULL;
        rn->home_vcpu = v->id;
        ui_dbg_deliver(rn, v->id, 4);
        ui_runq_insert_locked(v, rn);

        pthread_spin_unlock(&v->runq_lock);
        pthread_spin_unlock(&vic->runq_lock);
        return 1;
    }
    return 0;
}

static ui_vCPU *
ui_get_vcpu(void)
{
    return ui_this_vcpu;
}

static int
ui_get_vcpu_id(void)
{
    return ui_this_vcpu ? ui_this_vcpu->id : 0;
}

/* Wake every parked vCPU once the last goroutine retires.
 *
 * A vCPU only re-reads `running` after its block returns, so with a long
 * idle block timeout the last vCPU to park would otherwise sit out that
 * whole timeout before ui_Run()/ui_Fini() could join it.  vCPU 0 must be
 * included: ui_Run() runs vCPU 0 inline on the calling thread and only
 * signals the others after it returns, so vCPU 0 has nobody else to
 * wake it. */
static void
ui_goro_retired(void)
{
    if (atomic_load(&g_ui_sched.active_count) != 0)
        return;
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        uint64_t val = 1;
        if (g_ui_sched.vcpus[i].event_fd >= 0)
            (void)!write(g_ui_sched.vcpus[i].event_fd, &val, sizeof(val));
    }
}

/* Prepare a dead goroutine for pooling.  The stack handle is preserved
 * (only its physical pages are released, see ui_stack_trim);
 * everything else is reset so the goroutine can be handed out again.
 * Must be called while the goroutine is still private to this vCPU. */
static void
ui_goro_pool_prepare(ui_Goro *cg)
{
    uint64_t tok = cg->io_token;
    /* 不变量：带在途 I/O 的 goro 禁止入池。若放行，陈旧 CQE 会在地址
     * 复用且新住户再次置 io_pending 后命中（ABA），写入错误结果。
     * 正常路径不可能到达此处——I/O 等待循环退出时 io_pending 必为 0。 */
    if (cg->io_pending) {
        fprintf(stderr,
                "\nFATAL: recycling goro %p with in-flight I/O "
                "(io_pending=%d io_token=%llu state=%d)\n",
                (void *)cg, cg->io_pending,
                (unsigned long long)tok, cg->state);
        _exit(1);
    }
    void *sb = cg->stack_base;
    size_t sr = cg->stack_reserve;
    size_t sc = cg->stack_committed;
    ui_StackArena *sa = cg->stack_arena;
    int    ss = cg->stack_slot;
    int    ps = cg->page_size;
    /* The low-water mark belongs to the STACK, not to this life of the
     * goroutine: the next tenant of the same stack cannot use less than
     * the previous one, so keeping it lets the pool sweep trim just the
     * used pages as well. */
    uintptr_t slow = cg->stack_low;
    memset(cg, 0, sizeof(ui_Goro));
    cg->stack_base = sb;
    cg->stack_reserve = sr;
    cg->stack_committed = sc;
    cg->stack_arena = sa;
    cg->stack_slot = ss;
    cg->page_size = ps;
    cg->stack_low = slow;
    cg->sleepq_idx = -1;
    atomic_store(&cg->sleepq_vcpu, -1);
    cg->io_token = tok + 1;   /* 代际 +1：新世可被日志区分 */
    cg->state = UI_DEAD;   /* pooled goros must never look READY to stale wakes */
}

/* Hard-trim the stacks of this vCPU's pooled goroutines.
 *
 * With MADV_FREE the resident set of pooled stacks only shrinks when
 * the kernel reclaims the freed pages under memory pressure.  Sweeping
 * the pool every UI_STACK_TRIM_SWEEP_INTERVAL recycles keeps the
 * resident set close to the live set while costing (1 / interval) of a
 * MADV_DONTNEED per recycle.
 *
 * The window ROTATES: the pool is a LIFO stack (allocate pops the top,
 * recycle pushes it), so a fixed window would keep trimming the same
 * cold tail over and over - already clean after the first pass - and
 * never touch the stacks that were just recycled and still hold their
 * pages.
 *
 * Only this vCPU's own pool is swept: it is private to the vCPU, so no
 * lock is needed and no other vCPU can be looking at those stacks. */
#define UI_STACK_SWEEP_BATCH 64

static void
ui_goro_pool_sweep(ui_vCPU *v)
{
    if (!v)
        return;
    /* With NONE the pages are meant to stay, with DONTNEED they are
     * already gone: sweeping makes no sense for either. */
    if (ui_stack_trim_policy != UI_STACK_TRIM_FREE)
        return;
    int n = v->goro_pool_count;
    if (n <= 0)
        return;
    int batch = n > UI_STACK_SWEEP_BATCH ? UI_STACK_SWEEP_BATCH : n;
    if (v->trim_sweep_at >= n)
        v->trim_sweep_at = 0;
    if (v->trim_sweep_at + batch > n)
        batch = n - v->trim_sweep_at;
    for (int i = 0; i < batch; i++)
        ui_stack_hard_trim(v->goro_pool[v->trim_sweep_at + i]);
    v->trim_sweep_at += batch;
}

/* Would keeping this goroutine's stack in the pool exceed the memory
 * budget?  The budget covers the POOL only - the stacks of live
 * goroutines are not part of it. */
static int
ui_goro_pool_over_budget(unsigned long long used)
{
    if (!ui_stack_memory_limit)
        return 0;
    unsigned long long cur = __atomic_load_n(&ui_stack_pool_bytes,
                                             __ATOMIC_RELAXED);
    return cur + used > ui_stack_memory_limit;
}

/* Return a dead goro to a pool. Stacks preserved; physical pages released
 * per the trim policy. Must be called outside runq_lock.
 * Tries per-vCPU pool first (no lock), then global pool (mutex). */
static void
ui_goro_recycle(ui_Goro *cg)
{
    ui_vCPU *v = ui_get_vcpu();
    unsigned long long used = ui_goro_stack_bytes(cg);

    /* 1. Try per-vCPU pool (no lock) */
    if (v && v->goro_pool_count < UI_LOCAL_GORO_POOL_SIZE)
    {
        if (ui_goro_pool_over_budget(used))
        {
            /* The memory budget is exhausted: do not keep the stack.
             * Unmapping it is what actually returns the pages, so the
             * resident set follows the budget instead of the pool size. */
            ui_stack_destroy(cg);
            free(cg);
            atomic_fetch_sub(&g_ui_sched.active_count, 1);
            ui_goro_retired();
            return;
        }
        /* Release the stack's physical pages back to the kernel: pooled
         * goroutines keep their stacks for reuse, but retaining committed
         * pages makes RSS grow linearly with spawn count and eventually
         * OOMs long-running servers.  Pages fault back in on next use. */
        ui_stack_trim(cg);
        ui_goro_pool_prepare(cg);
        v->goro_pool[v->goro_pool_count] = cg;
        v->goro_pool_count++;
        __atomic_fetch_add(&ui_stack_pool_bytes, used, __ATOMIC_RELAXED);
        if (++v->trim_sweep_count >= UI_STACK_TRIM_SWEEP_INTERVAL)
        {
            v->trim_sweep_count = 0;
            ui_goro_pool_sweep(v);
        }
        atomic_fetch_sub(&g_ui_sched.active_count, 1);
        ui_goro_retired();
        return;
    }

    /* 2. Fall back to global pool (mutex).
     *
     * The stack pages are released BEFORE the pool lock is taken.  The
     * madvise() syscall costs ~10us; running it under goro_pool_lock
     * serialised every vCPU behind it (measured on a 20k-goroutine
     * spawn: 23k futex waits and a 14% latency regression).  Trimming
     * first is safe: the goroutine is still private to this vCPU, so no
     * other thread can reach the stack until it is published to the
     * pool below. */
    if (ui_goro_pool_over_budget(used))
    {
        ui_stack_destroy(cg);
        free(cg);
        atomic_fetch_sub(&g_ui_sched.active_count, 1);
        ui_goro_retired();
        return;
    }
    ui_stack_trim(cg);
    int pushed = 0;
    pthread_mutex_lock(&g_ui_sched.goro_pool_lock);
    if (g_ui_sched.goro_pool_count < UI_GORO_POOL_SIZE)
    {
        ui_goro_pool_prepare(cg);
        g_ui_sched.goro_pool[g_ui_sched.goro_pool_count] = cg;
        g_ui_sched.goro_pool_count++;
        pushed = 1;
    }
    pthread_mutex_unlock(&g_ui_sched.goro_pool_lock);
    if (pushed)
        __atomic_fetch_add(&ui_stack_pool_bytes, used, __ATOMIC_RELAXED);
    if (v && ++v->trim_sweep_count >= UI_STACK_TRIM_SWEEP_INTERVAL)
    {
        v->trim_sweep_count = 0;
        ui_goro_pool_sweep(v);
    }

    if (!pushed)
    {
        ui_stack_destroy(cg);
        free(cg);
    }
    atomic_fetch_sub(&g_ui_sched.active_count, 1);
    ui_goro_retired();
}

void
ui_schedule(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v) return;

    /* Detach the switching-out goro FIRST, before the standbyq drain /
     * sleepq expiry / io_uring drain below.  ui_uring_drain looks at
     * v->current to decide whether a completion belongs to a still-
     * running goro (its own wait-loop drain — no enqueue, it resumes
     * itself) or to a parked goro that must be delivered to a queue.
     * If v->current still pointed at the just-parked goro, the drain
     * would take the "current" branch and set state=UI_READY WITHOUT
     * enqueueing it: the goro is then runnable but in no queue, and
     * the voluntary-only re-insert below deliberately does not rescue
     * woken goros — lost forever (io_pending already 0, nothing ever
     * wakes it again).  With v->current == NULL the drains deliver
     * parked goros through their normal paths: same-vCPU runq insert
     * (with the FATAL verification in ui_uring_drain), cross-vCPU via
     * ui_wakeup/standbyq. */
    ui_Goro *cg = v->current;
    v->current = NULL;

    /* Remember the deepest stack address this goroutine was seen at.
     * cg->rsp holds the stack pointer it switched out with, so this is
     * a LOWER BOUND on the pages it touched, not the peak: a goroutine
     * that recursed and returned comes back near the top before it
     * switches out.  It feeds the stack memory budget's estimate and
     * nothing else - the trim always covers the committed region. */
    if (cg && cg->rsp && cg->stack_base)
    {
        uintptr_t sp = (uintptr_t)cg->rsp;
        uintptr_t base = (uintptr_t)cg->stack_base;
        uintptr_t top = base + cg->stack_reserve;
        if (sp >= base && sp <= top &&
            (cg->stack_low == 0 || sp < cg->stack_low))
            cg->stack_low = sp;
    }

    /* Drain standbyq (process cross-vCPU wakers/steals) */
    ui_drain_standbyq(v);
    if (atomic_load_explicit(&v->rm_closeq_len, memory_order_relaxed) > 0)
        ui_recvmulti_process_closeq(v);

    /* Expire sleepers so they become runnable (needed here, not just in idle
     * loop, to avoid deadlock when sleepq is full and no goroutine goes idle).
     * Skip clock read if sleepq is empty to avoid vDSO overhead. */
    if (atomic_load_explicit(&v->sleepq_len, memory_order_relaxed) > 0)
        ui_sleepq_expire(v, ui_now_us());
    if (v->uring_pending > 0 || v->has_multishot)
    {
        /* DEFER_TASKRUN: completions only arrive on explicit GETEVENTS.
         * Skip the syscall when the kernel says there is no deferred
         * task work to run (TASKRUN_FLAG) - the drain below still picks
         * up any CQE that is already in the ring. */
        if (ui_uring_taskrun_pending(v))
            ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
    }

    ui_Goro *g = NULL;

    /* Single lock region: re-insert READY goroutine + pick next.
     * Only re-insert goros that yielded VOLUNTARILY (ui_Yield / the
     * sleepq-full retry in ui_SleepUs).  A goro that parked
     * (state=WAITING) and was woken while switching (state now READY)
     * must NOT be re-inserted here: the waker owns its delivery (runq
     * insert or standbyq push), and re-inserting would double-reference
     * it — the standbyq drain's insert takes the HOME vCPU's runq_lock
     * while this one takes ours, so the g->prev guard in
     * ui_runq_insert_locked is not atomic across the two and both
     * inserts can land (two vCPUs running the same goro, work-stealing
     * FATAL / resurrected recycled goro).
     * The "completion raced the switch" case that fcc4739 dropped is
     * fixed at the source above: v->current is detached before
     * ui_uring_drain, so the drain delivers the parked goro itself
     * instead of taking the current-goro branch. */
    pthread_spin_lock(&v->runq_lock);

    if (cg && cg->state == UI_READY && cg->voluntary)
    {
        ui_dbg_deliver(cg, v->id, 6);
        ui_runq_insert_locked(v, cg);
        cg->voluntary = 0;
    }

    /* LIFO slot first (Go runnext / Tokio lifo_slot).  Anti-monopoly
     * cap: after UI_RUNNEXT_MAX_POLLS consecutive slot schedules, the
     * occupant is demoted to the FIFO tail and the counter resets —
     * ui has no sysmon preemption, so this is the fairness guard that
     * lets queued work interleave with a hot communicate-then-run pair. */
    int from_slot = 0;   /* UI_DEBUG_LIFO only */
    if (v->runnext)
    {
        if (v->runnext_polls < UI_RUNNEXT_MAX_POLLS)
        {
            g = v->runnext;
            v->runnext = NULL;
            v->runnext_polls++;
#ifdef UI_DEBUG_LIFO
            from_slot = 1;
#else
            (void)from_slot;
#endif
        }
        else
        {
            ui_dbg_deliver(v->runnext, v->id, 7);
            ui_runq_insert_locked(v, v->runnext);
            v->runnext = NULL;
            v->runnext_polls = 0;
        }
    }
    else
        v->runnext_polls = 0;

    while (!g && (g = v->runq_sentinel.next) != &v->runq_sentinel)
    {
        if (!g->prev || !g->next)
        {
            g = NULL;
            break;
        }
        g->prev->next = g->next;
        g->next->prev = g->prev;
        g->next = NULL;
        g->prev = NULL;
        atomic_fetch_sub(&v->runq_count, 1);
        if (g->state == UI_READY)
            break;
        g = NULL;
    }
    if (g == &v->runq_sentinel)
        g = NULL;

    if (!g)
    {
        /* no READY goroutine */
    }

    pthread_spin_unlock(&v->runq_lock);

    /* Outside lock: cleanup prev goroutine + run next */
    if (g)
    {
        if (cg)
        {
            if (cg->state == UI_DEAD)
            {
                ui_goro_recycle(cg);
            }
            /* WAITING: already removed from runq by blocking path */
        }

        /* Defensive: skip goros whose stack pointer was corrupted
         * (e.g. by stale CQE processing on recycled memory). */
        if (!g->first_run && !g->rsp) {
            g = NULL;
            goto no_goro;
        }

#ifdef UI_DEBUG_LIFO
        /* TEMP DEBUG (lifo investigation): double-run detector */
        if (g->state != UI_READY) {
            fprintf(stderr,
                "[dbg-2run] pick NON-READY src=%s g=%p state=%d\n",
                from_slot ? "slot" : "fifo", (void *)g, g->state);
            _exit(42);
        }
        for (int vi = 0; vi < g_ui_sched.nvcpus; vi++) {
            if (g_ui_sched.vcpus[vi].current == g &&
                g_ui_sched.vcpus[vi].id != v->id) {
                fprintf(stderr,
                    "[dbg-2run] pick g=%p ALSO CURRENT on vcpu %d "
                    "(picking vcpu %d src=%s writer=%d fill_state=%d "
                    "was_linked=%d vol=%d prev=%p next=%p)\n",
                    (void *)g, vi, v->id, from_slot ? "slot" : "fifo",
                    g->dbg_slot_writer, g->dbg_state_at_fill,
                    g->dbg_prev_voluntary, g->voluntary,
                    (void *)g->prev, (void *)g->next);
            fprintf(stderr,
                "[dbg-2run] deliveries: d0=(v%d,k%d) d1=(v%d,k%d)\n",
                g->dbg_d0v, g->dbg_d0k, g->dbg_d1v, g->dbg_d1k);
                _exit(43);
            }
        }
#endif

        g->state = UI_RUNNING;
        g->voluntary = 0;
        v->current = g;
        if (g->first_run)
        {
            /* A GoOn-pinned goro has now reached its requested vCPU;
             * from here on it participates in stealing like any other. */
            g->pinned = 0;
            g->first_run = 0;
            ui_first_switch(&v->sched_rsp, g->rsp);
        }
        else
        {
            /* This goro has now resumed on its requested home, so the
             * explicit binding has served its purpose: release it so
             * stealing can migrate the goro and, for a PinTo'd one, so
             * handoff may re-converge it again later.
             *
             * Both flags clear on the same condition, but they were set
             * for different reasons -- `pinned` by GoOn (which also grants
             * steal immunity), `bound` by ui_PinTo (which must not).  A
             * goro can only hold one of them at a time in practice, so
             * releasing either whenever the goro is home is correct and
             * costs nothing. */
            if (v->id == g->home_vcpu)
            {
                if (g->pinned) g->pinned = 0;
                if (g->bound)  g->bound = 0;
            }
            ui_switch(&v->sched_rsp, g->rsp);
        }
    }
    else
    {
no_goro:
        /* No goroutine to run */
        if (cg)
        {
            if (cg->state == UI_DEAD)
            {
                ui_goro_recycle(cg);
            }
        }
    }
}


static void
ui_sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)ctx;
    intptr_t fault = (intptr_t)info->si_addr;

#ifdef UI_DEBUG_LIFO
    /* TEMP DEBUG (lifo investigation): dump fault context */
    {
        ucontext_t *uc = (ucontext_t *)ctx;
        ui_vCPU *dv = ui_this_vcpu;
        fprintf(stderr,
            "[dbg-segv] sig=%d fault=%p rip=%p rsp=%p vcpu=%p cur=%p\n",
            sig, info->si_addr,
            (void *)uc->uc_mcontext.gregs[REG_RIP],
            (void *)uc->uc_mcontext.gregs[REG_RSP],
            (void *)dv, dv ? (void *)dv->current : NULL);
        if (dv && dv->current)
            fprintf(stderr,
                "[dbg-segv] entry=%p state=%d home=%d first_run=%d "
                "pinned=%d stack_base=%p\n",
                (void *)dv->current->entry,
                dv->current->state, dv->current->home_vcpu,
                dv->current->first_run, dv->current->pinned,
                dv->current->stack_base);
    }
#endif

    /* Only check the current vCPU — cross-vCPU access to v->current
     * races with ui_schedule's v->current = NULL assignment (C8). */
    ui_vCPU *v = ui_this_vcpu;
    if (v && v->current && v->current->stack_base)
    {
        intptr_t base = (intptr_t)v->current->stack_base;
        intptr_t top = base + (intptr_t)v->current->stack_reserve;

#ifdef UI_DEBUG_LIFO
        {
            ucontext_t *uc = (ucontext_t *)ctx;
            fprintf(stderr,
                "[dbg-segv] fault=%p rip=%p vcpu=%d cur=%p entry=%p "
                "state=%d home=%d first_run=%d pinned=%d\n",
                info->si_addr,
                (void *)uc->uc_mcontext.gregs[REG_RIP],
                v->id, (void *)v->current, (void *)v->current->entry,
                v->current->state, v->current->home_vcpu,
                v->current->first_run, v->current->pinned);
        }
#endif

        /* Include the slot's guard page (one page below base): a stack
         * overflow lands there when the committed region has reached
         * the reserve.  ui_stack_grow decides -2 (overflow, report)
         * vs -1 (not stack usage, re-raise). */
        if (fault >= base - (intptr_t)v->current->page_size && fault < top)
        {
            int ret = ui_stack_grow(v->current, info->si_addr);
            if (ret == 0) return;
            if (ret == -2) {
                static const char msg[] = "\nui stack overflow: exceeded 8MB limit\n";
                write(2, msg, sizeof(msg) - 1);
                _exit(1);
            }
        }
    }
    sigaction(SIGSEGV, &g_ui_sched.old_sigsegv, NULL);
    raise(SIGSEGV);
}

static int
ui_setup_signal_handler(void)
{
    void *altstack;
    struct sigaction sa;
    altstack = mmap(NULL, SIGSTKSZ, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (altstack == MAP_FAILED) return -1;
    g_ui_sched.old_altstack.ss_sp = altstack;
    g_ui_sched.old_altstack.ss_size = SIGSTKSZ;
    g_ui_sched.old_altstack.ss_flags = 0;
    if (sigaltstack(&g_ui_sched.old_altstack, NULL) < 0) { munmap(altstack, SIGSTKSZ); return -1; }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ui_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_ui_sched.old_sigsegv) < 0)
    {
        munmap(g_ui_sched.old_altstack.ss_sp, g_ui_sched.old_altstack.ss_size);
        memset(&g_ui_sched.old_altstack, 0, sizeof(g_ui_sched.old_altstack));
        return -1;
    }
    return 0;
}

static void
ui_teardown_signal_handler(void)
{
    sigaction(SIGSEGV, &g_ui_sched.old_sigsegv, NULL);
    if (g_ui_sched.old_altstack.ss_sp)
    {
        stack_t disabled;
        memset(&disabled, 0, sizeof(disabled));
        disabled.ss_flags = SS_DISABLE;
        sigaltstack(&disabled, NULL);
        munmap(g_ui_sched.old_altstack.ss_sp, g_ui_sched.old_altstack.ss_size);
        memset(&g_ui_sched.old_altstack, 0, sizeof(g_ui_sched.old_altstack));
    }
}

/* ── Stack memory policy (public API) ──
 *
 * Both setters may be called before ui_Init() (the environment is
 * applied by ui_Init(), an explicit call wins) or at any time after it
 * (the change takes effect on the next recycle). */

int
ui_SetStackTrimPolicy(ui_StackTrimPolicy policy)
{
    if (policy < UI_STACK_TRIM_NONE || policy > UI_STACK_TRIM_DONTNEED)
    {
        errno = EINVAL;
        return -1;
    }
    ui_stack_trim_policy_api = (int)policy;
    ui_stack_trim_policy = (int)policy;
    return 0;
}

int
ui_GetStackTrimPolicy(void)
{
    return ui_stack_trim_policy;
}

int
ui_SetStackMemoryLimit(size_t bytes)
{
    ui_stack_memory_limit_api = (unsigned long long)bytes;
    ui_stack_memory_limit_api_set = 1;
    ui_stack_memory_limit = (unsigned long long)bytes;
    /* A budget means "memory first": switch the advice to the strict
     * one unless the caller asked for a specific policy. */
    if (bytes && ui_stack_trim_policy_api < 0)
        ui_stack_trim_policy = UI_STACK_TRIM_DONTNEED;
    return 0;
}

size_t
ui_StackMemoryLimit(void)
{
    return (size_t)ui_stack_memory_limit;
}

/* Report the goroutine ceiling imposed by the kernel's VMA limit.
 *
 * Every goroutine stack costs about two VMAs (the arena is PROT_NONE and
 * each slot's guard page splits it in two), so the number of goroutines
 * that can be alive at the same time is roughly vm.max_map_count / 2.
 * The default (1048576) allows ~520k - plenty for most services, but a
 * container with a lowered limit would hit it much earlier, so say so
 * once at startup instead of failing later with ENOMEM. */
static void
ui_check_map_count_limit(void)
{
    FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
    if (!f)
        return;                     /* not Linux, or /proc not mounted */
    long long limit = 0;
    if (fscanf(f, "%lld", &limit) != 1)
        limit = 0;
    fclose(f);
    if (limit <= 0)
        return;
    long long goroutines = limit / 2;
    if (goroutines >= 100000)
        return;                     /* the usual limit is fine */
    fprintf(stderr,
            "ui: vm.max_map_count is %lld - at most ~%lld goroutines can be "
            "alive at the same time (each stack needs ~2 VMAs).\n"
            "    Raise it with 'sysctl vm.max_map_count=...' if the service "
            "needs more concurrency.\n",
            limit, goroutines);
}

int
ui_Init(void)
{
    if (g_ui_sched.initialized) return 0;
    memset(&g_ui_sched, 0, sizeof(g_ui_sched));
    pthread_mutex_init(&g_ui_sched.global_lock, NULL);
    pthread_mutex_init(&g_ui_sched.goro_pool_lock, NULL);
    pthread_mutex_init(&g_ui_sched.stack_arena_lock, NULL);
    if (ui_setup_signal_handler() < 0) return -1;
    int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;
    if (ncpus > UI_MAX_VCPUS) ncpus = UI_MAX_VCPUS;
    /* Allow override via environment for testing */
    const char *env_ncpus = getenv("UI_NVCPUS");
    if (env_ncpus) { int n = atoi(env_ncpus); if (n >= 1 && n <= UI_MAX_VCPUS) ncpus = n; }
    /* Stack trim policy.  UI_STACK_RELEASE_ON_RECYCLE was accepted but
     * never read by the recycle path, so it had no effect at all; it is
     * deprecated in favour of UI_STACK_TRIM, which selects what happens
     * to a recycled goroutine's pages (0 = keep, 1 = MADV_FREE,
     * 2 = MADV_DONTNEED).  The default is 0 (keep): see
     * UI_STACK_TRIM_DEFAULT for the measurements behind that. */
    const char *env_legacy = getenv("UI_STACK_RELEASE_ON_RECYCLE");
    const char *env_trim = getenv("UI_STACK_TRIM");
    int trim = UI_STACK_TRIM_DEFAULT;
    if (env_legacy)
    {
        fprintf(stderr,
                "ui: UI_STACK_RELEASE_ON_RECYCLE is deprecated and never had "
                "any effect; use UI_STACK_TRIM=0|1|2 instead"
                " (0=keep pages, 1=MADV_FREE, 2=MADV_DONTNEED)\n");
        if (!env_trim)
            trim = atoi(env_legacy) ? UI_STACK_TRIM_DONTNEED : UI_STACK_TRIM_NONE;
    }
    if (env_trim)
    {
        int n = atoi(env_trim);
        if (n >= UI_STACK_TRIM_NONE && n <= UI_STACK_TRIM_DONTNEED)
            trim = n;
    }
    /* An explicit ui_SetStackTrimPolicy() wins over the environment. */
    if (ui_stack_trim_policy_api >= 0)
        trim = ui_stack_trim_policy_api;
    ui_stack_trim_policy = trim;

    /* Memory budget for the goroutine pool (0 = no budget). */
    unsigned long long limit = 0;
    const char *env_limit = getenv("UI_STACK_MEMORY_LIMIT");
    if (env_limit)
    {
        long long v = atoll(env_limit);
        if (v > 0) limit = (unsigned long long)v;
    }
    if (ui_stack_memory_limit_api_set)
        limit = ui_stack_memory_limit_api;
    ui_stack_memory_limit = limit;
    /* A budget means "memory first": the fast advice (MADV_FREE) leaves
     * the pages resident, which is exactly what the budget is meant to
     * avoid.  An explicit ui_SetStackTrimPolicy() still wins. */
    if (limit && ui_stack_trim_policy_api < 0)
        ui_stack_trim_policy = UI_STACK_TRIM_DONTNEED;

    const char *env_yield = getenv("UI_YIELD_IO_MASK");
    if (env_yield) { int n = atoi(env_yield); if (n >= 0) ui_yield_io_mask = (uint32_t)n; }
    const char *env_idle = getenv("UI_IDLE_WAIT_MS");
    if (env_idle) { int n = atoi(env_idle); if (n >= 1) ui_idle_wait_ms = (uint32_t)n; }
    ui_check_map_count_limit();
    g_ui_sched.nvcpus = ncpus;
    atomic_store(&g_ui_sched.next_vcpu, 0);
    /* The vCPU array must start on a cache line: the structs are
     * cache-line aligned (see ui_vCPU) and malloc only guarantees 16
     * byte alignment. */
    {
        void *mem = NULL;
        size_t bytes = (size_t)ncpus * sizeof(ui_vCPU);
        if (posix_memalign(&mem, 64, bytes) != 0 || !mem)
        {
            ui_teardown_signal_handler();
            return -1;
        }
        memset(mem, 0, bytes);
        g_ui_sched.vcpus = (ui_vCPU *)mem;
    }
    if (!g_ui_sched.vcpus) { ui_teardown_signal_handler(); return -1; }
    for (int i = 0; i < ncpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        v->id = i;
        v->ring_fd = -1;
        v->bgid = -1;
        v->event_fd = eventfd(0, EFD_NONBLOCK);
        pthread_spin_init(&v->runq_lock, PTHREAD_PROCESS_PRIVATE);
        pthread_spin_init(&v->sleepq_lock, PTHREAD_PROCESS_PRIVATE);
        pthread_spin_init(&v->rm_closeq_lock, PTHREAD_PROCESS_PRIVATE);
        atomic_store(&v->rm_closeq_len, 0);
        atomic_store(&v->sleepq_len, 0);
        atomic_store(&v->sleepq_top_us, UINT64_MAX);
        ui_runq_init(v);
        ui_standbyq_init(v);
        atomic_store(&v->idle, 0);
        atomic_store(&v->running, 1);
        v->rng_state = (uint32_t)(i * 0x9E3779B9 + 1);
    }
    /* Pre-allocate a small warm pool; grow the cache lazily under load. */
    for (int i = 0; i < UI_GORO_PREALLOC; i++)
    {
        ui_Goro *g = calloc(1, sizeof(ui_Goro));
        if (!g) break;
        g->sleepq_idx = -1;
        atomic_store(&g->sleepq_vcpu, -1);
        if (ui_stack_init(g, UI_STACK_INIT) < 0) { free(g); break; }
        g_ui_sched.goro_pool[i] = g;
        g_ui_sched.goro_pool_count = i + 1;
    }
    g_ui_sched.initialized = 1;
    return 0;
}

void
ui_Fini(void)
{
    if (!g_ui_sched.initialized) return;
    g_ui_sched.finalizing = 1;
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        atomic_store(&g_ui_sched.vcpus[i].running, 0);
        pthread_spin_destroy(&g_ui_sched.vcpus[i].sleepq_lock);
        pthread_spin_destroy(&g_ui_sched.vcpus[i].rm_closeq_lock);
        if (g_ui_sched.vcpus[i].event_fd >= 0)
            close(g_ui_sched.vcpus[i].event_fd);
        /* Free active multishot recvs */
        {
            ui_vCPU *v = &g_ui_sched.vcpus[i];
            struct ui_RecvMulti *rm = v->active_multishot;
            while (rm)
            {
                struct ui_RecvMulti *next = rm->next;
                free(rm);
                rm = next;
            }
            v->active_multishot = NULL;
            rm = v->retired_multishot;
            while (rm)
            {
                struct ui_RecvMulti *next = rm->next;
                free(rm);
                rm = next;
            }
            v->retired_multishot = NULL;
        }
        ui_vcpu_destroy_buf_ring(&g_ui_sched.vcpus[i]);
        if (g_ui_sched.vcpus[i].sq_ring_ptr)
            munmap(g_ui_sched.vcpus[i].sq_ring_ptr, g_ui_sched.vcpus[i].sq_ring_size);
        if (g_ui_sched.vcpus[i].cq_ring_ptr)
            munmap(g_ui_sched.vcpus[i].cq_ring_ptr, g_ui_sched.vcpus[i].cq_ring_size);
        if (g_ui_sched.vcpus[i].sq_sqes)
            munmap(g_ui_sched.vcpus[i].sq_sqes, g_ui_sched.vcpus[i].sqes_size);
        if (g_ui_sched.vcpus[i].ring_fd >= 0)
            close(g_ui_sched.vcpus[i].ring_fd);
    }
    /* Drain per-vCPU pools + global pool */
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        for (int j = 0; j < v->goro_pool_count; j++)
        {
            ui_stack_destroy(v->goro_pool[j]);
            free(v->goro_pool[j]);
        }
        v->goro_pool_count = 0;
    }
    for (int i = 0; i < g_ui_sched.goro_pool_count; i++)
    {
        ui_stack_destroy(g_ui_sched.goro_pool[i]);
        free(g_ui_sched.goro_pool[i]);
    }
    g_ui_sched.goro_pool_count = 0;
    ui_stack_pool_bytes = 0;
    ui_stack_arenas_destroy();
    pthread_mutex_destroy(&g_ui_sched.stack_arena_lock);
    pthread_mutex_destroy(&g_ui_sched.goro_pool_lock);
    ui_teardown_signal_handler();
    free(g_ui_sched.vcpus);
    g_ui_sched.vcpus = NULL;
    g_ui_sched.nvcpus = 0;
    g_ui_sched.initialized = 0;
    g_ui_sched.finalizing = 0;
    ui_this_vcpu = NULL;
}

static void
ui_goro_init(ui_Goro *g)
{
    void *bottom = ui_stack_bottom(g);
    uint64_t *sp = (uint64_t *)bottom;
    *--sp = (uint64_t)(uintptr_t)ui_trampoline;
    *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)g->arg;
    *--sp = (uint64_t)(uintptr_t)g->entry;
    *--sp = 0;
    g->rsp = sp;
    g->state = UI_READY;
}

static ui_Goro *
ui_goro_alloc(ui_Func0 entry, void *arg, int stack_size)
{
    ui_vCPU *v = ui_get_vcpu();
    ui_Goro *g = NULL;

#ifdef UI_DEBUG_GORO_LIMIT
    /* Test hook: refuse goroutine allocations past a limit, so the
     * spawn-failure path can be exercised without exhausting the
     * address space. */
    if (ui_debug_goro_limit > 0 &&
        ++ui_debug_goro_alloc_count > ui_debug_goro_limit)
    {
        errno = ENOMEM;
        return NULL;
    }
#endif

    /* 1. Try current vCPU's per-vCPU pool (no lock) */
    if (v && v->goro_pool_count > 0)
    {
        v->goro_pool_count--;
        g = v->goro_pool[v->goro_pool_count];
        v->goro_pool[v->goro_pool_count] = NULL;
    }

    /* 2. Fall back to global pool (mutex) */
    if (!g)
    {
        pthread_mutex_lock(&g_ui_sched.goro_pool_lock);
        if (g_ui_sched.goro_pool_count > 0)
        {
            g_ui_sched.goro_pool_count--;
            g = g_ui_sched.goro_pool[g_ui_sched.goro_pool_count];
            g_ui_sched.goro_pool[g_ui_sched.goro_pool_count] = NULL;
        }
        pthread_mutex_unlock(&g_ui_sched.goro_pool_lock);
    }

    /* The goroutine came out of a pool: its stack is about to be reused,
     * so it no longer counts against the pool's memory budget. */
    if (g)
    {
        unsigned long long used = ui_goro_stack_bytes(g);
        if (used)
            __atomic_fetch_sub(&ui_stack_pool_bytes, used, __ATOMIC_RELAXED);
    }

    /* 3. Allocate new if nothing available */
    if (!g)
    {
        g = calloc(1, sizeof(ui_Goro));        if (!g) { errno = ENOMEM; return NULL; }
        g->sleepq_idx = -1;
        atomic_store(&g->sleepq_vcpu, -1);
        /* ui_stack_init() fails when the address space or the kernel's
         * VMA limit is exhausted; errno already carries the reason. */
        if (ui_stack_init(g, stack_size) < 0) { free(g); errno = ENOMEM; return NULL; }
    }

    g->entry = entry;
    g->arg = arg;
    g->state = UI_READY;
    g->pinned = 0;
    g->bound = 0;
    g->prev = NULL;
    g->next = NULL;
    g->free_next = NULL;
    g->standby_next = NULL;
    g->wait_node.active = 0;
    g->wait_node.g = NULL;
    g->io_pending = 0;
    g->chan_ptr = NULL;
    g->chan_recv_ptr = NULL;
    g->chan_send_ptr = NULL;
    g->chan_handoff = 0;

    ui_goro_init(g);
    g->first_run = 1;
    return g;
}

void
ui_goro_exit(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    v->current->state = UI_DEAD;
    ui_switch(&v->current->rsp, v->sched_rsp);
}

/* Enqueue a spawned goro on an explicit target vCPU (soft binding:
 * home_vcpu decides the initial runq placement and the wakeup/resume
 * point for every blocking path; work stealing may still migrate the
 * goro when the target goes idle, rewriting home_vcpu accordingly).
 * Out-of-range targets (including before ui_Init) clamp to vCPU 0. */
static void
ui_spawn_enqueue_at(ui_Goro *g, int target)
{
    if (target < 0 || target >= g_ui_sched.nvcpus)
        target = 0;

    g->home_vcpu = target;
    atomic_fetch_add(&g_ui_sched.active_count, 1);
    ui_dbg_deliver(g, target, 0);
    ui_runq_insert(&g_ui_sched.vcpus[target], g);
}

/* Place g into the vCPU's LIFO slot (Go runnext analog).  The previous
 * occupant is kicked to the FIFO tail — the newest readied goro runs
 * first, preserving "communicate-then-run" locality.  Slot mutations
 * take runq_lock; between unlocking and re-inserting `old` there is a
 * nanosecond window where it is reachable from no queue: harmless, it
 * is READY and merely delayed. */
static void
ui_runnext_put(ui_vCPU *v, ui_Goro *g)
{
    ui_Goro *old = NULL;
#ifdef UI_DEBUG_LIFO
    g->dbg_slot_writer = v->id;
    g->dbg_state_at_fill = g->state;
    g->dbg_prev_voluntary = g->prev ? 1 : 0;
#endif
    pthread_spin_lock(&v->runq_lock);
    old = v->runnext;
    v->runnext = g;
    pthread_spin_unlock(&v->runq_lock);

    if (old)
        ui_runq_insert(v, old);
}

static void
ui_spawn_enqueue(ui_Goro *g)
{
    int target;
    ui_vCPU *cur = ui_this_vcpu;

    if (cur && cur->current)
        target = cur->id;
    else if (!atomic_load(&g_ui_sched.started))
        target = 0;
    else if (g_ui_sched.nvcpus > 0)
    {
        int n = atomic_fetch_add(&g_ui_sched.next_vcpu, 1);
        if (n < 0) n = -n;
        target = n % g_ui_sched.nvcpus;
    }
    else
        target = 0;

    /* Same-vCPU spawn from a running goro feeds the LIFO slot instead
     * of the FIFO tail (newproc -> runqput(next=true) in Go). */
    if (cur && cur->current && target == cur->id && !g->pinned)
    {
        g->home_vcpu = target;
        atomic_fetch_add(&g_ui_sched.active_count, 1);
        ui_dbg_deliver(g, cur->id, 8);
        ui_runnext_put(cur, g);
    }
    else
        ui_spawn_enqueue_at(g, target);

    if (cur && cur->current && g_ui_sched.nvcpus > 1)
    {
        int peer = (target + 1) % g_ui_sched.nvcpus;
        if (peer != target)
            ui_wake_vcpu(&g_ui_sched.vcpus[peer]);
    }
    else
        ui_wake_vcpu(&g_ui_sched.vcpus[target]);
}

/* Spawn failure is fatal by default: a silently dropped task is a
 * correctness bug in the caller's workload, and the runtime cannot know
 * whether the task was optional.  Callers that can shed load use
 * ui_TryGo() instead. */
static void
ui_spawn_fatal(const char *what)
{
    fprintf(stderr,
            "\nFATAL: %s: cannot create goroutine: %s\n"
            "The runtime is out of goroutine slots or address space.\n"
            "Use ui_TryGo() if the caller can handle the failure.\n",
            what, strerror(errno));
    if (errno == ENOMEM)
        fprintf(stderr,
                "Hint: every goroutine stack costs ~2 VMAs (the arena is\n"
                "      PROT_NONE with a guard page per slot), so the\n"
                "      process runs into the kernel's VMA limit\n"
                "      (vm.max_map_count, 1048576 by default) at roughly\n"
                "      half that many live goroutines.  Raise it with\n"
                "      'sysctl vm.max_map_count=...' or keep fewer\n"
                "      goroutines alive at the same time.\n");
    _exit(1);
}

uint64_t ui_Go(ui_Func0 f) { return ui_GoSized(f, 0); }

uint64_t
ui_GoSized(ui_Func0 f, int stack_size)
{
    ui_Goro *g = ui_goro_alloc(f, NULL, stack_size);
    if (!g) ui_spawn_fatal("ui_Go");
    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
}

int
ui_TryGoSized(ui_Func0 f, int stack_size, uint64_t *id_out)
{
    ui_Goro *g = ui_goro_alloc(f, NULL, stack_size);
    if (!g) return -1;
    ui_spawn_enqueue(g);
    if (id_out) *id_out = (uint64_t)(uintptr_t)g;
    return 0;
}

int
ui_TryGo(ui_Func0 f, uint64_t *id_out)
{
    return ui_TryGoSized(f, 0, id_out);
}

uint64_t ui_Go1(void *fn, uintptr_t arg) { return ui_Go1Sized(fn, arg, 0); }

struct go1_pkg { void *fn; uintptr_t arg; };

static void
go1_trampoline(void *data)
{
    struct go1_pkg *p = data;
    ((void (*)(uintptr_t))p->fn)(p->arg);
    free(p);
}

uint64_t
ui_Go1Sized(void *fn, uintptr_t arg, int stack_size)
{
    struct go1_pkg *p = malloc(sizeof(*p));
    if (!p) { errno = ENOMEM; ui_spawn_fatal("ui_Go1"); }
    p->fn = fn; p->arg = arg;
    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, stack_size);
    if (!g) { free(p); ui_spawn_fatal("ui_Go1"); }
    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
}

int
ui_TryGo1(void *fn, uintptr_t arg, uint64_t *id_out)
{
    struct go1_pkg *p = malloc(sizeof(*p));
    if (!p) { errno = ENOMEM; return -1; }
    p->fn = fn; p->arg = arg;
    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, 0);
    if (!g) { free(p); errno = ENOMEM; return -1; }
    ui_spawn_enqueue(g);
    if (id_out) *id_out = (uint64_t)(uintptr_t)g;
    return 0;
}

/* ── Targeted spawn (GoOn) ──
 * Spawn on an explicit home vCPU: the goro is placed on that vCPU's runq
 * and resumes there after every blocking op (soft binding — a steal may
 * still migrate it, see ui_spawn_enqueue_at).  Out-of-range vcpu clamps
 * to 0. */

uint64_t ui_GoOn(ui_Func0 f, int vcpu) { return ui_GoOnSized(f, 0, vcpu); }

uint64_t
ui_GoOnSized(ui_Func0 f, int stack_size, int vcpu)
{
    ui_Goro *g = ui_goro_alloc(f, NULL, stack_size);
    if (!g) ui_spawn_fatal("ui_GoOn");
    g->pinned = 1;   /* honor first-run placement: not stealable yet */
    ui_spawn_enqueue_at(g, vcpu);
    ui_wake_vcpu(&g_ui_sched.vcpus[g->home_vcpu]);
    return (uint64_t)(uintptr_t)g;
}

uint64_t ui_Go1On(void *fn, uintptr_t arg, int vcpu) { return ui_Go1OnSized(fn, arg, 0, vcpu); }

uint64_t
ui_Go1OnSized(void *fn, uintptr_t arg, int stack_size, int vcpu)
{
    struct go1_pkg *p = malloc(sizeof(*p));
    if (!p) { errno = ENOMEM; ui_spawn_fatal("ui_Go1On"); }
    p->fn = fn; p->arg = arg;
    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, stack_size);
    if (!g) { free(p); ui_spawn_fatal("ui_Go1On"); }
    g->pinned = 1;   /* honor first-run placement: not stealable yet */
    ui_spawn_enqueue_at(g, vcpu);
    ui_wake_vcpu(&g_ui_sched.vcpus[g->home_vcpu]);
    return (uint64_t)(uintptr_t)g;
}

int
ui_NVCPUs(void)
{
    return g_ui_sched.nvcpus;
}

void
ui_Yield(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    v->current->state = UI_READY;
    v->current->voluntary = 1;
    ui_switch(&v->current->rsp, v->sched_rsp);
}

/* Rebind the CURRENT goro to an explicit home vCPU (soft binding): the goro
 * finishes its current quantum on this vCPU and, from its NEXT blocking
 * point on, is woken/resumed on the new home (chan/sync/select/io_uring
 * wakeups route via home_vcpu; ui_Sleep stays on the vCPU that submitted
 * it — per-VCPU sleepq).  Work stealing may still migrate the goro
 * afterwards.  Out-of-range vcpu clamps to 0.
 *
 * The binding is signalled with `bound`, not `pinned`: `pinned` also
 * exempts a goro from stealing (ui_steal_work, slot stealing), which
 * would contradict "work stealing may still migrate it" above -- and, since
 * the flag clears only when the goro next resumes on the requested vCPU, a
 * PinTo'd goro that keeps running on the current vCPU would stay exempt
 * forever.  `bound` is consulted only by channel direct handoff, whose
 * job is to pull a pair back onto the sender's core and so must not
 * silently undo a user-set home.
 *
 * Note the binding covers the window between this call and the goro's first
 * wakeup on `vcpu`; after that it is released and handoff may re-converge
 * the goro as it does for any other pair.
 */
void
ui_PinTo(int vcpu)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    if (vcpu < 0 || vcpu >= g_ui_sched.nvcpus) vcpu = 0;
    v->current->home_vcpu = vcpu;
    /* Stop channel direct handoff from pulling the goro back onto the
     * waker's core; released once it resumes on `vcpu` (ui_schedule). */
    v->current->bound = 1;
}

void
ui_Sleep(unsigned int ms)
{
    ui_SleepUs((unsigned int)ms * 1000);
}

void
ui_SleepUs(unsigned int us)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;

    uint64_t deadline = ui_now_us() + us;
    ui_Goro *g = v->current;

    /* If sleepq is full, yield and let the scheduler drain it, then retry */
    while (ui_sleepq_push(v, g, deadline) != 0)
    {
        g->state = UI_READY;
        g->voluntary = 1;
        ui_switch(&g->rsp, v->sched_rsp);
        v = ui_get_vcpu();
        if (!v || !v->current)
            return;
        g = v->current;
        deadline = ui_now_us() + us;
    }

    g->state = UI_WAITING;
    /* Switch to scheduler (cleanup will remove from runq) */
    ui_switch(&g->rsp, v->sched_rsp);
}

/* Route a goro whose state has already been transitioned to UI_READY
 * onto its home vCPU: same-vCPU feeds the LIFO slot, cross-vCPU goes
 * through standbyq (avoids the cross-vCPU runq race and cross-vCPU
 * sleepq access — the target drains and removes from its own sleepq).
 * Shared by ui_wakeup and ui_wakeup_handoff. */
static void
ui_wake_enqueue(ui_Goro *g)
{
    int target = g->home_vcpu;
    if (target < 0 || target >= g_ui_sched.nvcpus) target = 0;

    ui_vCPU *v_target = &g_ui_sched.vcpus[target];

    /* The sleep queue may belong to a different vCPU than the wake
     * destination (PinTo); remove under the owner's lock first. */
    if (atomic_load_explicit(&g->sleepq_vcpu, memory_order_relaxed) >= 0)
        ui_sleepq_remove(g);

    if (ui_get_vcpu_id() == target)
    {
        /* Same-vCPU wakeup feeds the LIFO slot ("whoever woke me runs
         * next"), unless the goro is pinned to its first placement. */
        ui_vCPU *cur = ui_this_vcpu;
        if (cur && cur->current && !g->pinned) {
            ui_dbg_deliver(g, cur->id, 8);
            ui_runnext_put(cur, g);
        } else {
            ui_dbg_deliver(g, target, 10);
            ui_runq_insert(v_target, g);
        }
    }
    else
    {
        ui_standbyq_push(v_target, g);
        ui_wake_vcpu(v_target);
    }
}

void
ui_wakeup(ui_Goro *g)
{
    ui_dbg_check_not_current(g);
    if (!g) return;
    if (!__sync_bool_compare_and_swap(&g->state, UI_WAITING, UI_READY))
        return;

    ui_wake_enqueue(g);
}

/* Wake a channel direct-handoff partner onto the WAKER's LIFO slot.
 * This is the re-convergence step that keeps a ping-pong pair glued to
 * one vCPU: even if the pair was split earlier by stealing, every
 * completed handoff pulls the partner back to the waker's core
 * (Go: goready -> ready(gp, next=true) -> runqput(next=true)).
 *
 * Only do this when the waker's vCPU is already the partner's home.  The
 * partner may have published WAITING and its wait node, but not yet reached
 * its own ui_switch: its saved rsp still belongs to the PREVIOUS park.  If a
 * different vCPU ran it now it would resume from the stale rsp while the
 * original vCPU is still executing the park path -- the same goro running
 * twice (and later reusing/madvising its stack).  Cross-vCPU wakes therefore
 * route through ui_wake_enqueue(), which targets home_vcpu; that vCPU is the
 * one currently running the partner, so it cannot pick it up until the park
 * switch has completed.
 *
 * Explicitly bound goros keep their targeted placement and route via
 * ui_wake_enqueue instead: `pinned` (GoOn first-run pin) or `bound`
 * (ui_PinTo).
 * Locking note: callers hold their channel spinlock; nesting
 * ch-lock -> runq_lock already exists via plain ui_wakeup and no code
 * path takes them in reverse order. */
void
ui_wakeup_handoff(ui_Goro *g)
{
    ui_dbg_check_not_current(g);
    if (!g) return;
    if (!__sync_bool_compare_and_swap(&g->state, UI_WAITING, UI_READY))
        return;

    ui_vCPU *cur = ui_this_vcpu;
    if (cur && cur->current && !g->pinned && !g->bound &&
        cur->id == g->home_vcpu && cur->id < g_ui_sched.nvcpus)
    {
        ui_dbg_deliver(g, cur->id, 9);
        ui_runnext_put(cur, g);
        return;
    }

    /* Cross-vCPU or bound goro: honor g->home_vcpu.  Note the state CAS
     * above already succeeded, so we must enqueue directly rather than call
     * ui_wakeup (whose own CAS would fail and silently drop the wakeup). */
    ui_wake_enqueue(g);
}

void
ui_vcpu_idle(ui_vCPU *v)
{
    atomic_store(&v->idle, 0);

    /* Try to find work without touching io_uring or clock */
    if (!ui_runq_empty(v)) return;
    if (ui_steal_work(v)) return;

    if (atomic_load_explicit(&v->rm_closeq_len, memory_order_relaxed) > 0)
        ui_recvmulti_process_closeq(v);

    /* Drain io_uring completions if any are pending */
    if (v->uring_pending > 0 || v->has_multishot) {
        if (ui_uring_taskrun_pending(v))
            ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
        if (!ui_runq_empty(v)) return;
    }

    /* Compute max block time. Read clock once — sleepq_expire
     * and wait_us calculation share the same timestamp. */
    uint64_t now = ui_now_us();
    ui_sleepq_expire(v, now);

    uint64_t wait_us = (uint64_t)ui_idle_wait_ms * 1000;

    /* The block covers every wake source: event_fd is written on
     * cross-vCPU wakeup AND (once the ring registered it) on every I/O
     * completion.  So the timeout no longer has to be chopped down to
     * poll for steal or for I/O — it is only a lost-wakeup watchdog.
     *
     * The exception is a vCPU with a live ring but no event_fd
     * registration (old kernel, or registration failed): there, nothing
     * signals an I/O completion, so a long block would delay it by the
     * whole timeout.  Fall back to a short poll in that case. */
    if (v->ring_fd >= 0 && !v->cq_eventfd)
        wait_us = 1000;

    uint64_t top_us = atomic_load_explicit(&v->sleepq_top_us,
                                           memory_order_relaxed);
    if (top_us != UINT64_MAX && top_us > now)
    {
        uint64_t delta = top_us - now;
        if (delta > 10000000) delta = 10000000;
        if (delta < wait_us)
            wait_us = delta;
    }

    /* Brief spin before blocking: wakes that arrive while we are still
     * on-CPU are consumed via standbyq drains with no eventfd write and
     * no ppoll syscall round trip (the big cost of cross-vCPU wakeups
     * under mutex/channel contention).  We are NOT idle yet, so wakers
     * skip the eventfd write entirely and just push to the standbyq.
     * The racy standbyq_head load keeps the loop cheap — the drain
     * revalidates under the lock. */
    for (int i = 0; i < UI_IDLE_SPIN_ITERS; i++)
    {
        __builtin_ia32_pause();
        if (i & 7) continue;
        if (v->standbyq_head)
        {
            ui_drain_standbyq(v);
            if (!ui_runq_empty(v)) return;
        }
        if (v->uring_pending > 0 || v->has_multishot)
        {
            if (ui_uring_taskrun_pending(v))
                ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
            ui_uring_drain(v);
            if (!ui_runq_empty(v)) return;
        }
    }

    /* Set idle flag and do final check before blocking.
     * drain_standbyq catches cross-vCPU wakeups; steal_work
     * catches work that appeared on other vCPUs. */
    atomic_store(&v->idle, 1);
    ui_drain_standbyq(v);
    ui_sleepq_expire(v, ui_now_us());
    if (!ui_runq_empty(v) || ui_steal_work(v))
    {
        atomic_store(&v->idle, 0);
        return;
    }
    if (v->uring_pending > 0 || v->has_multishot) {
        if (ui_uring_taskrun_pending(v))
            ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
        if (!ui_runq_empty(v))
        {
            atomic_store(&v->idle, 0);
            return;
        }
    }

    /* Block until work arrives or timeout */
    ui_uring_idle_wait(v, wait_us);
    atomic_store(&v->idle, 0);
}
