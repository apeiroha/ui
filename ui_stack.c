#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define ALIGN_UP(x, a) (((x) + (a)-1) & ~((a)-1))
#define UI_STACK_ARENA_SLOTS 64

struct ui_StackArena
{
    void *base;
    size_t reserve;
    int slots;
    uint64_t free_mask;
    ui_StackArena *next;
};

static int
ui_stack_init_individual(ui_Goro *g, size_t reserve, size_t commit, int page_size)
{
    void *base = mmap(NULL, reserve, PROT_NONE,
                      MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (base == MAP_FAILED)
        return -1;

    void *commit_start = (char *)base + reserve - commit;
    if (mprotect(commit_start, commit, PROT_READ | PROT_WRITE) < 0)
    {
        munmap(base, reserve);
        return -1;
    }

    g->stack_base = base;
    g->stack_reserve = reserve;
    g->stack_committed = commit;
    g->stack_arena = NULL;
    g->stack_slot = -1;
    g->page_size = page_size;
    return 0;
}

static int
ui_stack_init_arena(ui_Goro *g, size_t reserve, size_t commit, int page_size)
{
    ui_StackArena *arena = NULL;
    int slot = -1;

    pthread_mutex_lock(&g_ui_sched.stack_arena_lock);
    for (arena = g_ui_sched.stack_arenas; arena; arena = arena->next)
    {
        if (arena->free_mask)
            break;
    }
    if (!arena)
    {
        size_t map_size = reserve * UI_STACK_ARENA_SLOTS;
        void *base = mmap(NULL, map_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
        if (base != MAP_FAILED)
        {
            arena = calloc(1, sizeof(*arena));
            if (arena)
            {
                arena->base = base;
                arena->reserve = reserve;
                arena->slots = UI_STACK_ARENA_SLOTS;
                arena->free_mask = UINT64_MAX;
                arena->next = g_ui_sched.stack_arenas;
                g_ui_sched.stack_arenas = arena;
            }
            else
            {
                munmap(base, map_size);
            }
        }
    }
    if (arena && arena->free_mask)
    {
        slot = __builtin_ctzll(arena->free_mask);
        arena->free_mask &= ~(1ULL << slot);
    }
    pthread_mutex_unlock(&g_ui_sched.stack_arena_lock);

    if (!arena || slot < 0)
        return -1;

    void *base = (char *)arena->base + (size_t)slot * reserve;
    void *commit_start = (char *)base + reserve - commit;
    if (mprotect(commit_start, commit, PROT_READ | PROT_WRITE) < 0)
    {
        pthread_mutex_lock(&g_ui_sched.stack_arena_lock);
        arena->free_mask |= 1ULL << slot;
        pthread_mutex_unlock(&g_ui_sched.stack_arena_lock);
        return -1;
    }

    g->stack_base = base;
    g->stack_reserve = reserve;
    g->stack_committed = commit;
    g->stack_arena = arena;
    g->stack_slot = slot;
    g->page_size = page_size;
    return 0;
}

int
ui_stack_init(ui_Goro *g, int stack_size)
{
    long page_size;
    size_t reserve, commit;

    page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0)
        page_size = UI_PAGE_SIZE;

    if (stack_size <= 0)
        stack_size = UI_STACK_INIT;

    reserve = ALIGN_UP(UI_STACK_RESERVE, page_size);
    commit = ALIGN_UP((size_t)stack_size, page_size);
    if (commit >= reserve)
        commit = reserve / 2;

    if (ui_stack_init_arena(g, reserve, commit, (int)page_size) == 0)
        return 0;
    return ui_stack_init_individual(g, reserve, commit, (int)page_size);
}

void
ui_stack_destroy(ui_Goro *g)
{
    if (g->stack_arena && g->stack_slot >= 0)
    {
        if (!g_ui_sched.finalizing)
        {
            mprotect(g->stack_base, g->stack_reserve, PROT_NONE);
            pthread_mutex_lock(&g_ui_sched.stack_arena_lock);
            g->stack_arena->free_mask |= 1ULL << g->stack_slot;
            pthread_mutex_unlock(&g_ui_sched.stack_arena_lock);
        }
    }
    else if (g->stack_base && g->stack_reserve > 0)
    {
        munmap(g->stack_base, g->stack_reserve);
    }
    g->stack_base = NULL;
    g->stack_reserve = 0;
    g->stack_committed = 0;
    g->stack_arena = NULL;
    g->stack_slot = -1;
}

void
ui_stack_arenas_destroy(void)
{
    pthread_mutex_lock(&g_ui_sched.stack_arena_lock);
    ui_StackArena *arena = g_ui_sched.stack_arenas;
    g_ui_sched.stack_arenas = NULL;
    pthread_mutex_unlock(&g_ui_sched.stack_arena_lock);

    while (arena)
    {
        ui_StackArena *next = arena->next;
        munmap(arena->base, arena->reserve * (size_t)arena->slots);
        free(arena);
        arena = next;
    }
}

/* Release physical pages of the committed region; keep virtual address range.
 * Call when returning a stack to the pool. */
void
ui_stack_madvise_dontneed(ui_Goro *g)
{
    if (!g->stack_base || g->stack_committed == 0)
        return;
    void *commit_start = (char *)g->stack_base + g->stack_reserve - g->stack_committed;
    madvise(commit_start, g->stack_committed, MADV_DONTNEED);
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
    return (char *)g->stack_base + g->stack_reserve;
}
