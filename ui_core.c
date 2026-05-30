#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>

ui_Sched g_ui_sched;

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

static int
ui_get_vcpu_id(void)
{
    pthread_t self = pthread_self();
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        if (pthread_equal(g_ui_sched.vcpus[i].thread, self))
            return i;
    }
    return 0;
}

static int
ui_enqueue_local(ui_vCPU *v, ui_Goro *g)
{
    int tail = atomic_load(&v->runq_tail);
    int head = atomic_load(&v->runq_head);
    if (tail - head < UI_RUNQ_CAP)
    {
        v->runq[tail % UI_RUNQ_CAP] = g;
        atomic_store(&v->runq_tail, tail + 1);
        return 0;
    }
    return -1;
}

static int
ui_enqueue_global(ui_Goro *g)
{
    pthread_mutex_lock(&g_ui_sched.global_lock);
    if (!g_ui_sched.global_tail)
        g_ui_sched.global_head = g;
    else
        g_ui_sched.global_tail->runq_next = g;
    g->runq_next = NULL;
    g_ui_sched.global_tail = g;
    pthread_mutex_unlock(&g_ui_sched.global_lock);
    return 0;
}

static ui_Goro *
ui_dequeue_local(ui_vCPU *v)
{
    int head = atomic_load(&v->runq_head);
    int tail = atomic_load(&v->runq_tail);
    if (head < tail)
    {
        ui_Goro *g = v->runq[head % UI_RUNQ_CAP];
        atomic_store(&v->runq_head, head + 1);
        return g;
    }
    return NULL;
}

static ui_Goro *
ui_dequeue_global(void)
{
    pthread_mutex_lock(&g_ui_sched.global_lock);
    ui_Goro *g = g_ui_sched.global_head;
    if (g)
    {
        g_ui_sched.global_head = g->runq_next;
        if (!g_ui_sched.global_head)
            g_ui_sched.global_tail = NULL;
        g->runq_next = NULL;
    }
    pthread_mutex_unlock(&g_ui_sched.global_lock);
    return g;
}

static ui_Goro *
ui_steal_work(ui_vCPU *v)
{
    for (int attempt = 0; attempt < g_ui_sched.nvcpus * 2; attempt++)
    {
        int vid = rand() % g_ui_sched.nvcpus;
        if (vid == v->id)
            continue;

        ui_vCPU *victim = &g_ui_sched.vcpus[vid];
        int vhead = atomic_load(&victim->runq_head);
        int vtail = atomic_load(&victim->runq_tail);
        int n = vtail - vhead;

        if (n > 1)
        {
            int steal_n = n / 2;
            /* Try to CAS the head forward */
            if (atomic_compare_exchange_strong(&victim->runq_head, &vhead, vhead + steal_n))
            {
                for (int i = 0; i < steal_n; i++)
                {
                    ui_Goro *sg = victim->runq[(vhead + i) % UI_RUNQ_CAP];
                    int t = atomic_load(&v->runq_tail);
                    int h = atomic_load(&v->runq_head);
                    if (t - h < UI_RUNQ_CAP)
                    {
                        v->runq[t % UI_RUNQ_CAP] = sg;
                        atomic_store(&v->runq_tail, t + 1);
                    }
                    else
                    {
                        ui_enqueue_global(sg);
                    }
                }
                return ui_dequeue_local(v);
            }
        }
    }
    return NULL;
}

void
ui_schedule(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v)
        return;

    /* Try local, global, steal */
    ui_Goro *g = ui_dequeue_local(v);
    if (!g)
        g = ui_dequeue_global();
    if (!g)
        g = ui_steal_work(v);

    if (!g)
        return;

    g->state = UI_RUNNING;
    v->current = g;

    ui_switch(&v->sched_rsp, g->rsp);

    v->current = NULL;

    if (g->state == UI_DEAD)
    {
        if (g->joiner)
        {
            g->joiner->state = UI_READY;
            if (ui_enqueue_local(v, g->joiner) < 0)
                ui_enqueue_global(g->joiner);
            g->joiner = NULL;
        }
        ui_stack_destroy(g);
        pthread_mutex_lock(&g_ui_sched.free_lock);
        g->wait_next = g_ui_sched.free_list;
        g_ui_sched.free_list = g;
        pthread_mutex_unlock(&g_ui_sched.free_lock);
        atomic_fetch_sub(&g_ui_sched.active_count, 1);
    }
    else if (g->state == UI_READY)
    {
        if (ui_enqueue_local(v, g) < 0)
            ui_enqueue_global(g);
    }
}

static void
ui_sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig;
    (void)ctx;

    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        if (v->current && v->current->stack_base)
        {
            intptr_t base = (intptr_t)v->current->stack_base;
            intptr_t top = base + (intptr_t)v->current->stack_reserve;
            intptr_t fault = (intptr_t)info->si_addr;
            if (fault >= base && fault < top)
            {
                int ret = ui_stack_grow(v->current, info->si_addr);
                if (ret == 0)
                    return;
                if (ret == -2)
                {
                    fprintf(stderr, "\nui stack overflow: exceeded 8MB limit\n"
                            "  fault=%p\n", info->si_addr);
                    _exit(1);
                }
                break;
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
    if (altstack == MAP_FAILED)
        return -1;

    g_ui_sched.old_altstack.ss_sp = altstack;
    g_ui_sched.old_altstack.ss_size = SIGSTKSZ;
    g_ui_sched.old_altstack.ss_flags = 0;
    if (sigaltstack(&g_ui_sched.old_altstack, NULL) < 0)
    {
        munmap(altstack, SIGSTKSZ);
        return -1;
    }

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
        munmap(g_ui_sched.old_altstack.ss_sp, g_ui_sched.old_altstack.ss_size);
        memset(&g_ui_sched.old_altstack, 0, sizeof(g_ui_sched.old_altstack));
    }
}

int
ui_Init(void)
{
    if (g_ui_sched.initialized)
        return 0;

    memset(&g_ui_sched, 0, sizeof(g_ui_sched));
    pthread_mutex_init(&g_ui_sched.global_lock, NULL);
    pthread_mutex_init(&g_ui_sched.free_lock, NULL);

    if (ui_setup_signal_handler() < 0)
        return -1;

    int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;
    if (ncpus > UI_MAX_VCPUS) ncpus = UI_MAX_VCPUS;

    g_ui_sched.nvcpus = ncpus;
    g_ui_sched.vcpus = calloc((size_t)ncpus, sizeof(ui_vCPU));
    if (!g_ui_sched.vcpus)
    {
        ui_teardown_signal_handler();
        return -1;
    }

    for (int i = 0; i < ncpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        v->id = i;
        v->event_fd = eventfd(0, EFD_NONBLOCK);
        atomic_store(&v->running, 1);
    }

    g_ui_sched.initialized = 1;
    return 0;
}

void
ui_Fini(void)
{
    if (!g_ui_sched.initialized)
        return;

    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        atomic_store(&g_ui_sched.vcpus[i].running, 0);
        if (g_ui_sched.vcpus[i].event_fd >= 0)
            close(g_ui_sched.vcpus[i].event_fd);
    }

    ui_teardown_signal_handler();
    free(g_ui_sched.vcpus);
    g_ui_sched.vcpus = NULL;
    g_ui_sched.nvcpus = 0;
    g_ui_sched.initialized = 0;
}

static void
ui_goro_init(ui_Goro *g)
{
    void *bottom = ui_stack_bottom(g);
    uint64_t *sp = (uint64_t *)bottom;

    *--sp = (uint64_t)(uintptr_t)ui_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)g->arg;
    *--sp = (uint64_t)(uintptr_t)g->entry;
    *--sp = 0;

    g->rsp = sp;
    g->state = UI_READY;
}

static ui_Goro *
ui_goro_alloc(ui_Func0 entry, void *arg, int stack_size)
{
    ui_Goro *g;

    pthread_mutex_lock(&g_ui_sched.free_lock);
    g = g_ui_sched.free_list;
    if (g)
        g_ui_sched.free_list = g->wait_next;
    pthread_mutex_unlock(&g_ui_sched.free_lock);

    if (!g)
    {
        g = calloc(1, sizeof(ui_Goro));
        if (!g)
            return NULL;
    }

    g->entry = entry;
    g->arg = arg;

    if (ui_stack_init(g, stack_size) < 0)
    {
        free(g);
        return NULL;
    }

    ui_goro_init(g);
    return g;
}

void
ui_goro_exit(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current)
        return;

    ui_Goro *g = v->current;
    g->state = UI_DEAD;

    v->current = NULL;
    ui_switch(&g->rsp, v->sched_rsp);
}

static int
ui_pick_vcpu(void)
{
    int caller = ui_get_vcpu_id();
    /* Put on caller's vCPU if possible, otherwise round-robin */
    int target = atomic_load(&g_ui_sched.vcpus[caller].runq_tail) -
                 atomic_load(&g_ui_sched.vcpus[caller].runq_head);
    if (target < UI_RUNQ_CAP)
        return caller;
    return rand() % g_ui_sched.nvcpus;
}

static void
ui_spawn_enqueue(ui_Goro *g)
{
    g->home_vcpu = ui_pick_vcpu();
    ui_vCPU *v = &g_ui_sched.vcpus[g->home_vcpu];

    atomic_fetch_add(&g_ui_sched.active_count, 1);

    if (ui_enqueue_local(v, g) < 0)
        ui_enqueue_global(g);

    /* Kick the target vCPU if it might be idle */
    uint64_t val = 1;
    write(v->event_fd, &val, sizeof(val));
}

uint64_t
ui_Go(ui_Func0 f)
{
    return ui_GoSized(f, 0);
}

uint64_t
ui_GoSized(ui_Func0 f, int stack_size)
{
    ui_Goro *g = ui_goro_alloc(f, NULL, stack_size);
    if (!g)
        return 0;

    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
}

uint64_t
ui_Go1(void *fn, uintptr_t arg)
{
    return ui_Go1Sized(fn, arg, 0);
}

struct go1_pkg
{
    void     *fn;
    uintptr_t arg;
};

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
    if (!p)
        return 0;
    p->fn = fn;
    p->arg = arg;

    ui_Goro *g = ui_goro_alloc((ui_Func0)go1_trampoline, p, stack_size);
    if (!g)
    {
        free(p);
        return 0;
    }

    ui_spawn_enqueue(g);
    return (uint64_t)(uintptr_t)g;
}

void
ui_Yield(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current)
        return;

    ui_Goro *g = v->current;
    g->state = UI_READY;
    ui_switch(&g->rsp, v->sched_rsp);
}

void
ui_Sleep(unsigned int ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    struct timespec now;
    do {
        ui_Yield();
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
}

void
ui_wakeup(ui_Goro *g)
{
    if (!g)
        return;

    g->state = UI_READY;

    int target = g->home_vcpu;
    if (target < 0 || target >= g_ui_sched.nvcpus)
        target = 0;

    ui_vCPU *v_target = &g_ui_sched.vcpus[target];

    if (ui_enqueue_local(v_target, g) < 0)
        ui_enqueue_global(g);

    uint64_t val = 1;
    write(v_target->event_fd, &val, sizeof(val));
}

static void
ui_vcpu_idle(ui_vCPU *v)
{
    struct pollfd pfd = { .fd = v->event_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN))
    {
        uint64_t val;
        read(v->event_fd, &val, sizeof(val));
    }
}

static void *
ui_vcpu_main(void *arg)
{
    ui_vCPU *v = arg;

    while (atomic_load(&v->running))
    {
        v->tick++;
        ui_schedule();

        if (!v->current)
        {
            /* Check if all work is done */
            if (atomic_load(&g_ui_sched.active_count) == 0 &&
                atomic_load(&v->runq_head) >= atomic_load(&v->runq_tail))
            {
                /* Try global queue one more time */
                ui_Goro *gg = ui_dequeue_global();
                if (!gg)
                {
                    atomic_store(&v->running, 0);
                    break;
                }
                /* Put it back */
                ui_enqueue_global(gg);
                continue;
            }
            ui_vcpu_idle(v);
        }
    }
    return NULL;
}

void
ui_Run(void)
{
    if (g_ui_sched.nvcpus == 0)
        return;

    /* vCPU 0 runs on calling thread */
    ui_vCPU *main_v = &g_ui_sched.vcpus[0];
    main_v->thread = pthread_self();

    /* Start other vCPUs */
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        pthread_create(&v->thread, NULL, ui_vcpu_main, v);
    }

    /* Run vCPU 0 scheduler */
    ui_vcpu_main(main_v);

    /* Signal all vCPUs to stop and join */
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
    {
        atomic_store(&g_ui_sched.vcpus[i].running, 0);
        uint64_t val = 1;
        write(g_ui_sched.vcpus[i].event_fd, &val, sizeof(val));
        pthread_join(g_ui_sched.vcpus[i].thread, NULL);
    }
}
