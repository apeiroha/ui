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
ui_enqueue_local(ui_vCPU *v, ui_Goro *g)
{
    if (v->runq_tail - v->runq_head < UI_RUNQ_CAP)
    {
        int idx = v->runq_tail % UI_RUNQ_CAP;
        v->runq[idx] = g;
        v->runq_tail++;
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
    if (v->runq_head < v->runq_tail)
    {
        int idx = v->runq_head % UI_RUNQ_CAP;
        ui_Goro *g = v->runq[idx];
        v->runq_head++;
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

void
ui_schedule(void)
{
    ui_vCPU *v = ui_get_vcpu();
    if (!v)
        return;

    /* Try local queue, then global */
    ui_Goro *g = ui_dequeue_local(v);
    if (!g)
        g = ui_dequeue_global();

    if (!g)
        return;

    g->state = UI_RUNNING;
    v->current = g;

    /* Save scheduler context (v->sched_rsp), load goroutine context (g->rsp) */
    ui_switch(&v->sched_rsp, g->rsp);

    /* Goroutine yielded, we're back.
     * g->rsp now holds goroutine's yield point. */
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
    }
    else if (g->state == UI_READY)
    {
        /* yield: re-enqueue */
        if (ui_enqueue_local(v, g) < 0)
            ui_enqueue_global(g);
    }
    else if (g->state == UI_WAITING)
    {
        /* waiting: don't enqueue — stayed in wait list */
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

    g_ui_sched.nvcpus = 1;
    g_ui_sched.vcpus = calloc(g_ui_sched.nvcpus, sizeof(ui_vCPU));
    if (!g_ui_sched.vcpus)
    {
        ui_teardown_signal_handler();
        return -1;
    }

    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        ui_vCPU *v = &g_ui_sched.vcpus[i];
        v->id = i;
        v->event_fd = eventfd(0, EFD_NONBLOCK);
        v->running = 1;
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
        g_ui_sched.vcpus[i].running = 0;
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

    /* Stack layout for first switch:
     *   [rbp = 0]
     *   [rbx = entry]
     *   [r12 = arg]
     *   [r13 = 0]
     *   [r14 = 0]
     *   [r15 = 0]
     *   [ret = ui_trampoline]
     * Trampoline: mov r12->rdi; call *rbx; jmp ui_goro_exit
     */
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

    /* Switch back to scheduler. The scheduler will handle cleanup. */
    v->current = NULL;
    ui_switch(&g->rsp, v->sched_rsp);
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

    if (ui_enqueue_local(&g_ui_sched.vcpus[0], g) < 0)
        ui_enqueue_global(g);

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

    if (ui_enqueue_local(&g_ui_sched.vcpus[0], g) < 0)
        ui_enqueue_global(g);

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
    /* Save goroutine context, load scheduler context */
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

    if (ui_enqueue_local(&g_ui_sched.vcpus[0], g) < 0)
        ui_enqueue_global(g);

    /* Kick eventfd if vCPU might be idle */
    int fd = g_ui_sched.vcpus[0].event_fd;
    if (fd >= 0)
    {
        uint64_t val = 1;
        write(fd, &val, sizeof(val));
    }
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

static int
ui_has_work(ui_vCPU *v)
{
    if (v->runq_head < v->runq_tail)
        return 1;

    pthread_mutex_lock(&g_ui_sched.global_lock);
    int has = (g_ui_sched.global_head != NULL);
    pthread_mutex_unlock(&g_ui_sched.global_lock);
    return has;
}

static void *
ui_vcpu_main(void *arg)
{
    ui_vCPU *v = arg;

    while (v->running)
    {
        v->tick++;
        ui_schedule();

        if (!v->current)
        {
            if (!ui_has_work(v))
            {
                /* No goroutines ready — check if any exist */
                pthread_mutex_lock(&g_ui_sched.global_lock);
                int exists = (g_ui_sched.global_head != NULL);
                pthread_mutex_unlock(&g_ui_sched.global_lock);
                if (!exists && v->runq_head >= v->runq_tail)
                {
                    /* No goroutines at all — exit */
                    v->running = 0;
                    break;
                }
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

    ui_vCPU *main_v = &g_ui_sched.vcpus[0];
    main_v->thread = pthread_self();

    ui_vcpu_main(main_v);
}
