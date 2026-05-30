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

void
ui_runq_init(ui_vCPU *v)
{
    v->runq_sentinel.next = &v->runq_sentinel;
    v->runq_sentinel.prev = &v->runq_sentinel;
}

void
ui_runq_insert(ui_vCPU *v, ui_Goro *g)
{
    ui_Goro *s = &v->runq_sentinel;
    ui_Goro *last = s->prev;
    g->next = s;
    g->prev = last;
    last->next = g;
    s->prev = g;
}

void
ui_runq_remove(ui_Goro *g)
{
    g->prev->next = g->next;
    g->next->prev = g->prev;
    g->next = NULL;
    g->prev = NULL;
}

int
ui_runq_empty(ui_vCPU *v)
{
    return v->runq_sentinel.next == &v->runq_sentinel;
}

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

void
ui_schedule(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v) return;

    ui_Goro *g = NULL;

    if (v->current && v->current->state == UI_READY)
    {
        g = v->current->next;
        if (g == &v->runq_sentinel) g = g->next;
        if (g == &v->runq_sentinel) g = NULL;
    }

    if (!g)
    {
        g = v->runq_sentinel.next;
        if (g == &v->runq_sentinel) g = NULL;
    }

    if (g)
    {
        g->state = UI_RUNNING;
        v->current = g;
        if (g->first_run)
        {
            g->first_run = 0;
            ui_first_switch(&v->sched_rsp, g->rsp);
        }
        else
            ui_switch(&v->sched_rsp, g->rsp);
    }

    if (v->current)
    {
        ui_Goro *cg = v->current;
        v->current = NULL;
        if (cg->state == UI_DEAD)
        {
            if (cg->joiner)
            {
                cg->joiner->state = UI_READY;
                ui_runq_insert(&g_ui_sched.vcpus[cg->joiner->home_vcpu], cg->joiner);
                cg->joiner = NULL;
            }
            ui_runq_remove(cg);
            ui_stack_destroy(cg);
            pthread_mutex_lock(&g_ui_sched.free_lock);
            cg->wq_next = g_ui_sched.free_list;
            g_ui_sched.free_list = cg;
            pthread_mutex_unlock(&g_ui_sched.free_lock);
            atomic_fetch_sub(&g_ui_sched.active_count, 1);
        }
        else if (cg->state == UI_WAITING)
            ui_runq_remove(cg);
    }
}

static void
ui_sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)ctx;
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
                if (ret == 0) return;
                if (ret == -2) {
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
    pthread_mutex_init(&g_ui_sched.free_lock, NULL);
    if (ui_setup_signal_handler() < 0) return -1;
    int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;
    if (ncpus > UI_MAX_VCPUS) ncpus = UI_MAX_VCPUS;
    g_ui_sched.nvcpus = ncpus;
    g_ui_sched.vcpus = calloc((size_t)ncpus, sizeof(ui_vCPU));
    if (!g_ui_sched.vcpus) { ui_teardown_signal_handler(); return -1; }
    for (int i = 0; i < ncpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        v->id = i;
        v->event_fd = eventfd(0, EFD_NONBLOCK);
        ui_runq_init(v);
        atomic_store(&v->running, 1);
    }
    g_ui_sched.initialized = 1;
    return 0;
}

void
ui_Fini(void)
{
    if (!g_ui_sched.initialized) return;
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
    ui_Goro *g;
    pthread_mutex_lock(&g_ui_sched.free_lock);
    g = g_ui_sched.free_list;
    if (g) g_ui_sched.free_list = g->wq_next;
    pthread_mutex_unlock(&g_ui_sched.free_lock);
    if (!g) { g = calloc(1, sizeof(ui_Goro)); if (!g) return NULL; }
    g->entry = entry;
    g->arg = arg;
    if (ui_stack_init(g, stack_size) < 0) { free(g); return NULL; }
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
    /* Don't clear v->current — scheduler needs it for cleanup */
    ui_switch(&v->current->rsp, v->sched_rsp);
}

static void
ui_spawn_enqueue(ui_Goro *g)
{
    g->home_vcpu = ui_get_vcpu_id();
    atomic_fetch_add(&g_ui_sched.active_count, 1);
    ui_runq_insert(&g_ui_sched.vcpus[g->home_vcpu], g);
    uint64_t val = 1;
    write(g_ui_sched.vcpus[g->home_vcpu].event_fd, &val, sizeof(val));
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

void
ui_Yield(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v || !v->current) return;
    v->current->state = UI_READY;
    ui_switch(&v->current->rsp, v->sched_rsp);
}

void
ui_Sleep(unsigned int ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    struct timespec now;
    do { ui_Yield(); clock_gettime(CLOCK_MONOTONIC, &now); }
    while (now.tv_sec < deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
}

void
ui_wakeup(ui_Goro *g)
{
    if (!g) return;
    g->state = UI_READY;
    int target = g->home_vcpu;
    if (target < 0 || target >= g_ui_sched.nvcpus) target = 0;
    ui_runq_insert(&g_ui_sched.vcpus[target], g);
    uint64_t val = 1;
    write(g_ui_sched.vcpus[target].event_fd, &val, sizeof(val));
}

static void
ui_vcpu_idle(ui_vCPU *v)
{
    struct pollfd pfd = { .fd = v->event_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 100);
    if (ret > 0 && (pfd.revents & POLLIN))
    { uint64_t val; read(v->event_fd, &val, sizeof(val)); }
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
            if (atomic_load(&g_ui_sched.active_count) == 0 && ui_runq_empty(v))
            { atomic_store(&v->running, 0); break; }
            ui_vcpu_idle(v);
        }
    }
    return NULL;
}

void
ui_Run(void)
{
    if (g_ui_sched.nvcpus == 0) return;
    ui_vCPU *main_v = &g_ui_sched.vcpus[0];
    main_v->thread = pthread_self();
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
        pthread_create(&g_ui_sched.vcpus[i].thread, NULL, ui_vcpu_main, &g_ui_sched.vcpus[i]);
    ui_vcpu_main(main_v);
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
    {
        atomic_store(&g_ui_sched.vcpus[i].running, 0);
        uint64_t val = 1;
        write(g_ui_sched.vcpus[i].event_fd, &val, sizeof(val));
        pthread_join(g_ui_sched.vcpus[i].thread, NULL);
    }
}
