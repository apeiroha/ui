#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define ALIGN_UP(x, a) (((x) + (a)-1) & ~((a)-1))

int
ui_stack_init(ui_Goro *g, int stack_size)
{
    long page_size;
    size_t reserve, commit;
    void *base, *commit_start;

    page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0)
        page_size = UI_PAGE_SIZE;

    if (stack_size <= 0)
        stack_size = UI_STACK_INIT;

    reserve = ALIGN_UP(UI_STACK_RESERVE, page_size);
    commit = ALIGN_UP((size_t)stack_size, page_size);
    if (commit >= reserve)
        commit = reserve / 2;

    base = mmap(NULL, reserve, PROT_NONE,
                MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (base == MAP_FAILED)
        return -1;

    commit_start = base + reserve - commit;
    if (mprotect(commit_start, commit, PROT_READ | PROT_WRITE) < 0)
    {
        munmap(base, reserve);
        return -1;
    }

    g->stack_base = base;
    g->stack_reserve = reserve;
    g->stack_committed = commit;
    g->page_size = (int)page_size;
    return 0;
}

void
ui_stack_destroy(ui_Goro *g)
{
    if (g->stack_base && g->stack_reserve > 0)
        munmap(g->stack_base, g->stack_reserve);
    g->stack_base = NULL;
    g->stack_reserve = 0;
    g->stack_committed = 0;
}

int
ui_stack_grow(ui_Goro *g, void *fault_addr)
{
    intptr_t base, top, fault, commit_start;
    size_t grow_needed, new_committed;

    if (!g || !fault_addr)
        return -1;

    base = (intptr_t)g->stack_base;
    top = base + (intptr_t)g->stack_reserve;
    fault = (intptr_t)fault_addr;

    if (fault < base || fault >= top)
        return -1;

    commit_start = top - (intptr_t)g->stack_committed;
    if (fault >= commit_start)
        return -1;

    grow_needed = (size_t)(commit_start - fault) + (size_t)g->page_size;
    grow_needed = ALIGN_UP(grow_needed, (size_t)g->page_size);

    new_committed = g->stack_committed + grow_needed;
    if (new_committed > g->stack_reserve)
        return -2;

    if (mprotect((void *)(top - (intptr_t)new_committed),
                 new_committed, PROT_READ | PROT_WRITE) < 0)
        return -1;

    g->stack_committed = new_committed;
    return 0;
}

void *
ui_stack_bottom(ui_Goro *g)
{
    return g->stack_base + g->stack_reserve;
}
