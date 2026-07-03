#include "ui_internal.h"

#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

/* vCPU startup is compiled at -O0 (separate TU) to prevent LLVM's
 * Attributor pass from applying !callback metadata propagation to
 * pthread_create's 4th argument (&g_ui_sched.vcpus[i]), which is
 * miscompiled at -O1+ in PIE mode (LLVM bug).
 *
 * The index (not pointer) is passed to prevent the compiler from
 * tracing the argument through the callback boundary. */
static void *
ui_vcpu_main(void *arg)
{
    int idx = (int)(intptr_t)arg;
    ui_vCPU *v = &g_ui_sched.vcpus[idx];
    stack_t ss;
    void *altstack = NULL;

    if (idx != 0)
    {
        altstack = mmap(NULL, SIGSTKSZ, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
        if (altstack != MAP_FAILED)
        {
            ss.ss_sp = altstack;
            ss.ss_size = SIGSTKSZ;
            ss.ss_flags = 0;
            sigaltstack(&ss, NULL);
        }
        else
            altstack = NULL;
    }

    v->thread = pthread_self();
    ui_this_vcpu = v;
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
    if (altstack)
    {
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        munmap(altstack, SIGSTKSZ);
    }
    return NULL;
}

void
ui_Run(void)
{
    if (g_ui_sched.nvcpus == 0) return;
    atomic_store(&g_ui_sched.started, 1);
    g_ui_sched.vcpus[0].thread = pthread_self();
    ui_this_vcpu = &g_ui_sched.vcpus[0];
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
    {
        pthread_create(&g_ui_sched.vcpus[i].thread, NULL,
                       ui_vcpu_main, (void*)(intptr_t)i);
    }
    ui_vcpu_main((void*)(intptr_t)0);
    for (int i = 1; i < g_ui_sched.nvcpus; i++)
    {
        atomic_store(&g_ui_sched.vcpus[i].running, 0);
        uint64_t val = 1;
        write(g_ui_sched.vcpus[i].event_fd, &val, sizeof(val));
        pthread_join(g_ui_sched.vcpus[i].thread, NULL);
    }
    atomic_store(&g_ui_sched.started, 0);
}
