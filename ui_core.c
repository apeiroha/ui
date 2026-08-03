#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

static void
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
            if (head->sleepq_idx >= 0)
                ui_sleepq_remove(v, head);
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

int
ui_sleepq_push(ui_vCPU *v, ui_Goro *g, uint64_t deadline_us)
{
    if (v->sleepq_size >= 256)
        return -1;
    g->wakeup_time = deadline_us;
    g->sleepq_idx = v->sleepq_size;
    v->sleepq[v->sleepq_size] = g;
    v->sleepq_size++;
    sleepq_sift_up(v, g->sleepq_idx);
    return 0;
}

void
ui_sleepq_remove(ui_vCPU *v, ui_Goro *g)
{
    int idx = g->sleepq_idx;
    if (idx < 0 || idx >= v->sleepq_size || v->sleepq[idx] != g)
        return;
    v->sleepq_size--;
    if (idx < v->sleepq_size)
    {
        v->sleepq[idx] = v->sleepq[v->sleepq_size];
        v->sleepq[idx]->sleepq_idx = idx;
        sleepq_sift_down(v, idx);
        sleepq_sift_up(v, idx);
    }
    g->sleepq_idx = -1;
}

ui_Goro *
ui_sleepq_pop(ui_vCPU *v)
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
    return g;
}

int
ui_sleepq_expire(ui_vCPU *v, uint64_t now_us)
{
    int count = 0;
    while (v->sleepq_size > 0 && v->sleepq[0]->wakeup_time <= now_us)
    {
        ui_Goro *g = ui_sleepq_pop(v);
        g->state = UI_READY;
        ui_runq_insert(v, g);
        count++;
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
        if (count > 2)
        {
            int steal_n = count / 2;
            ui_Goro *batch_end = s->prev;
            ui_Goro *batch_start = batch_end;
            for (int i = 1; i < steal_n; i++)
                batch_start = batch_start->prev;

            batch_start->prev->next = s;
            s->prev = batch_start->prev;
            atomic_fetch_sub(&vic->runq_count, steal_n);

            ui_Goro *steal = batch_start;
            for (;;)
            {
                ui_Goro *next = steal->next;
                steal->next = NULL;
                steal->prev = NULL;
                steal->home_vcpu = v->id;
                ui_runq_insert_locked(v, steal);
                if (steal == batch_end) break;
                steal = next;
            }
            ok = 1;
        }

        pthread_spin_unlock(&v->runq_lock);
        pthread_spin_unlock(&vic->runq_lock);
        if (ok) return 1;
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

/* Return a dead goro to a pool. Stacks preserved; physical pages released
 * via MADV_DONTNEED. Must be called outside runq_lock.
 * Tries per-vCPU pool first (no lock), then global pool (mutex). */
static void
ui_goro_recycle(ui_Goro *cg)
{
    if (cg->joiner)
    {
        cg->joiner->state = UI_READY;
        ui_runq_insert(&g_ui_sched.vcpus[cg->joiner->home_vcpu], cg->joiner);
        cg->joiner = NULL;
    }

    ui_vCPU *v = ui_get_vcpu();

    /* 1. Try per-vCPU pool (no lock) */
    if (v && v->goro_pool_count < UI_LOCAL_GORO_POOL_SIZE)
    {
        if (g_ui_sched.release_stacks_on_recycle)
            ui_stack_madvise_dontneed(cg);
        void *sb = cg->stack_base;
        size_t sr = cg->stack_reserve;
        size_t sc = cg->stack_committed;
        ui_StackArena *sa = cg->stack_arena;
        int    ss = cg->stack_slot;
        int    ps = cg->page_size;
        memset(cg, 0, sizeof(ui_Goro));
        cg->stack_base = sb;
        cg->stack_reserve = sr;
        cg->stack_committed = sc;
        cg->stack_arena = sa;
        cg->stack_slot = ss;
        cg->page_size = ps;
        cg->sleepq_idx = -1;
        cg->state = UI_DEAD;   /* pooled goros must never look READY to stale wakes */
        v->goro_pool[v->goro_pool_count] = cg;
        v->goro_pool_count++;
        atomic_fetch_sub(&g_ui_sched.active_count, 1);
        return;
    }

    /* 2. Fall back to global pool (mutex) */
    int pushed = 0;
    pthread_mutex_lock(&g_ui_sched.goro_pool_lock);
    if (g_ui_sched.goro_pool_count < UI_GORO_POOL_SIZE)
    {
        if (g_ui_sched.release_stacks_on_recycle)
            ui_stack_madvise_dontneed(cg);
        void *sb = cg->stack_base;
        size_t sr = cg->stack_reserve;
        size_t sc = cg->stack_committed;
        ui_StackArena *sa = cg->stack_arena;
        int    ss = cg->stack_slot;
        int    ps = cg->page_size;
        memset(cg, 0, sizeof(ui_Goro));
        cg->stack_base = sb;
        cg->stack_reserve = sr;
        cg->stack_committed = sc;
        cg->stack_arena = sa;
        cg->stack_slot = ss;
        cg->page_size = ps;
        cg->sleepq_idx = -1;
        cg->state = UI_DEAD;   /* pooled goros must never look READY to stale wakes */
        g_ui_sched.goro_pool[g_ui_sched.goro_pool_count] = cg;
        g_ui_sched.goro_pool_count++;
        pushed = 1;
    }
    pthread_mutex_unlock(&g_ui_sched.goro_pool_lock);

    if (!pushed)
    {
        ui_stack_destroy(cg);
        free(cg);
    }
    atomic_fetch_sub(&g_ui_sched.active_count, 1);
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

    /* Drain standbyq (process cross-vCPU wakers/steals) */
    ui_drain_standbyq(v);

    /* Expire sleepers so they become runnable (needed here, not just in idle
     * loop, to avoid deadlock when sleepq is full and no goroutine goes idle).
     * Skip clock read if sleepq is empty to avoid vDSO overhead. */
    if (v->sleepq_size > 0)
        ui_sleepq_expire(v, ui_now_us());
    if (v->uring_pending > 0 || v->has_multishot)
    {
        /* DEFER_TASKRUN: completions only arrive on explicit GETEVENTS */
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
        ui_runq_insert_locked(v, cg);
        cg->voluntary = 0;
    }

    while ((g = v->runq_sentinel.next) != &v->runq_sentinel)
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

        g->state = UI_RUNNING;
        g->voluntary = 0;
        v->current = g;
        if (g->first_run)
        {
            g->first_run = 0;
            ui_first_switch(&v->sched_rsp, g->rsp);
        }
        else
            ui_switch(&v->sched_rsp, g->rsp);
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

    /* Only check the current vCPU — cross-vCPU access to v->current
     * races with ui_schedule's v->current = NULL assignment (C8). */
    ui_vCPU *v = ui_this_vcpu;
    if (v && v->current && v->current->stack_base)
    {
        intptr_t base = (intptr_t)v->current->stack_base;
        intptr_t top = base + (intptr_t)v->current->stack_reserve;
        intptr_t fault = (intptr_t)info->si_addr;
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
    const char *env_release = getenv("UI_STACK_RELEASE_ON_RECYCLE");
    g_ui_sched.release_stacks_on_recycle = env_release && atoi(env_release) != 0;
    const char *env_yield = getenv("UI_YIELD_IO_MASK");
    if (env_yield) { int n = atoi(env_yield); if (n >= 0) ui_yield_io_mask = (uint32_t)n; }
    g_ui_sched.nvcpus = ncpus;
    atomic_store(&g_ui_sched.next_vcpu, 0);
    g_ui_sched.vcpus = calloc((size_t)ncpus, sizeof(ui_vCPU));
    if (!g_ui_sched.vcpus) { ui_teardown_signal_handler(); return -1; }
    for (int i = 0; i < ncpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        v->id = i;
        v->ring_fd = -1;
        v->bgid = -1;
        v->event_fd = eventfd(0, EFD_NONBLOCK);
        pthread_spin_init(&v->runq_lock, PTHREAD_PROCESS_PRIVATE);
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

    /* 3. Allocate new if nothing available */
    if (!g)
    {
        g = calloc(1, sizeof(ui_Goro));
        if (!g) return NULL;
        g->sleepq_idx = -1;
        if (ui_stack_init(g, stack_size) < 0) { free(g); return NULL; }
    }

    g->entry = entry;
    g->arg = arg;
    g->state = UI_READY;
    g->prev = NULL;
    g->next = NULL;
    g->free_next = NULL;
    g->standby_next = NULL;
    g->joiner = NULL;
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
    ui_runq_insert(&g_ui_sched.vcpus[target], g);
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

uint64_t ui_Go(ui_Func0 f) { return ui_GoSized(f, 0); }

uint64_t
ui_GoSized(ui_Func0 f, int stack_size)
{
    ui_Goro *g = ui_goro_alloc(f, NULL, stack_size);
    if (!g) return 0;
    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
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
    if (!p) return 0;
    p->fn = fn; p->arg = arg;
    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, stack_size);
    if (!g) { free(p); return 0; }
    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
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
    if (!g) return 0;
    ui_spawn_enqueue_at(g, vcpu);
    ui_wake_vcpu(&g_ui_sched.vcpus[g->home_vcpu]);
    return (uint64_t)(uintptr_t)g;
}

uint64_t ui_Go1On(void *fn, uintptr_t arg, int vcpu) { return ui_Go1OnSized(fn, arg, 0, vcpu); }

uint64_t
ui_Go1OnSized(void *fn, uintptr_t arg, int stack_size, int vcpu)
{
    struct go1_pkg *p = malloc(sizeof(*p));
    if (!p) return 0;
    p->fn = fn; p->arg = arg;
    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, stack_size);
    if (!g) { free(p); return 0; }
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

/* Rebind the CURRENT goro to an explicit home vCPU (soft binding, same
 * semantics as GoOn): the goro finishes its current quantum on this vCPU
 * and, from its NEXT blocking point on, is woken/resumed on the new home
 * (chan/sync/select/io_uring wakeups route via home_vcpu; ui_Sleep stays
 * on the vCPU that submitted it — per-vCPU sleepq).  Work stealing may
 * still migrate the goro afterwards.  Out-of-range vcpu clamps to 0. */
void
ui_PinTo(int vcpu)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    if (vcpu < 0 || vcpu >= g_ui_sched.nvcpus) vcpu = 0;
    v->current->home_vcpu = vcpu;
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
        /* Woken up — retry sleep */
        deadline = ui_now_us() + us;
    }

    g->state = UI_WAITING;
    /* Switch to scheduler (cleanup will remove from runq) */
    ui_switch(&g->rsp, v->sched_rsp);
}

void
ui_wakeup(ui_Goro *g)
{
    if (!g) return;
    if (!__sync_bool_compare_and_swap(&g->state, UI_WAITING, UI_READY))
        return;

    int target = g->home_vcpu;
    if (target < 0 || target >= g_ui_sched.nvcpus) target = 0;

    ui_vCPU *v_target = &g_ui_sched.vcpus[target];

    /* Same-vCPU: remove from sleepq and insert directly into runq.
     * Cross-vCPU: push to standbyq (avoids cross-vCPU runq race AND
     * cross-vCPU sleepq access).  The target vCPU's standbyq drain
     * will remove from the sleepq locally. */
    if (ui_get_vcpu_id() == target)
    {
        if (g->sleepq_idx >= 0)
            ui_sleepq_remove(v_target, g);
        ui_runq_insert(v_target, g);
    }
    else
    {
        ui_standbyq_push(v_target, g);
        ui_wake_vcpu(v_target);
    }
}

void
ui_vcpu_idle(ui_vCPU *v)
{
    atomic_store(&v->idle, 0);

    /* Try to find work without touching io_uring or clock */
    if (!ui_runq_empty(v)) return;
    if (ui_steal_work(v)) return;

    /* Drain io_uring completions if any are pending */
    if (v->uring_pending > 0 || v->has_multishot) {
        ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
        if (!ui_runq_empty(v)) return;
    }

    /* Compute max block time. Read clock once — sleepq_expire
     * and wait_us calculation share the same timestamp. */
    uint64_t now = ui_now_us();
    ui_sleepq_expire(v, now);

    uint64_t wait_us = 100000; /* 100ms default */
    if (atomic_load(&g_ui_sched.active_count) > 0)
        wait_us = 1000; /* active runtime: poll for steal every 1ms */
    if (v->sleepq_size > 0 && v->sleepq[0]->wakeup_time > now)
    {
        uint64_t delta = v->sleepq[0]->wakeup_time - now;
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
