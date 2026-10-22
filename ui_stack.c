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
    size_t stride;  /* reserve + page_size: per-slot stride incl. guard page */
    int slots;
    uint64_t free_mask;
    size_t *slot_commit;  /* per slot: bytes already mapped RW from the top */
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
        /* Each slot is reserve + one guard page: the guard page sits at
         * the slot's bottom and stays PROT_NONE forever, so a stack
         * overflow past the reserve faults on the guard instead of
         * silently walking into the adjacent slot's committed region. */
        size_t stride = reserve + (size_t)page_size;
        size_t map_size = stride * UI_STACK_ARENA_SLOTS;
        void *base = mmap(NULL, map_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
        if (base != MAP_FAILED)
        {
            arena = calloc(1, sizeof(*arena));
            if (arena)
                arena->slot_commit = calloc((size_t)UI_STACK_ARENA_SLOTS,
                                            sizeof(size_t));
            if (arena && arena->slot_commit)
            {
                arena->base = base;
                arena->reserve = reserve;
                arena->stride = stride;
                arena->slots = UI_STACK_ARENA_SLOTS;
                arena->free_mask = UINT64_MAX;
                arena->next = g_ui_sched.stack_arenas;
                g_ui_sched.stack_arenas = arena;
            }
            else
            {
                free(arena);
                arena = NULL;
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

    /* Slot layout: [slot_start, slot_start + page_size) = PROT_NONE
     * guard page, [slot_start + page_size, +stride) = the stack. */
    void *base = (char *)arena->base + (size_t)slot * arena->stride
               + (size_t)page_size;
    void *commit_start = (char *)base + reserve - commit;
    /* Only extend the RW region; a reused slot may already have a larger
     * commit from a previous (bigger) stack.  A bit was not enough here:
     * reusing a 64KB slot for 1MB used to skip the mprotect and then fault
     * inside the range the goro believed was committed. */
    if (arena->slot_commit[slot] < commit)
    {
        if (mprotect(commit_start, commit, PROT_READ | PROT_WRITE) < 0)
        {
            pthread_mutex_lock(&g_ui_sched.stack_arena_lock);
            arena->free_mask |= 1ULL << slot;
            pthread_mutex_unlock(&g_ui_sched.stack_arena_lock);
            return -1;
        }
        arena->slot_commit[slot] = commit;
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
            /* Keep the slot's pages warm; slot_commit records how much
             * is actually RW so a larger reuse re-mprotects the delta. */
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
        munmap(arena->base, arena->stride * (size_t)arena->slots);
        free(arena->slot_commit);
        free(arena);
        arena = next;
    }
}

/* Hand the physical pages of the committed region back to the kernel.
 *
 * Only the part that was actually touched is returned: the goroutine
 * records the lowest stack address it was ever seen at (stack_low), and
 * the pages below it were never faulted in.  A shallow goroutine uses a
 * couple of pages, so this trims ~16x less memory than the whole
 * committed region (128KB by default) - and the madvise cost is
 * proportional to the range that is walked. */
static void
ui_stack_trim_range(ui_Goro *g, int advice)
{
    if (!g->stack_base || g->stack_committed == 0)
        return;
    char *base = (char *)g->stack_base;
    char *top  = base + g->stack_reserve;
    char *start = top - g->stack_committed;      /* committed region start */
    if (g->stack_low)
    {
        char *low = (char *)g->stack_low;
        if (low > start && low < top)
        {
            /* madvise() needs a page-aligned start; round DOWN so the
             * page that holds the deepest used address is included. */
            size_t ps = (size_t)(g->page_size > 0 ? g->page_size : UI_PAGE_SIZE);
            start = (char *)((uintptr_t)low & ~((uintptr_t)ps - 1));
        }
    }
    if (start >= top)
        return;
    madvise(start, (size_t)(top - start), advice);
}

/* Release the physical pages of the committed region; keep the virtual
 * address range.  Called when returning a stack to the pool.
 *
 * Which advice is used depends on ui_stack_trim_policy:
 *   MADV_FREE     - the pages are marked free and reclaimed lazily by the
 *                   kernel.  No TLB shootdown, so recycling a goroutine
 *                   is cheaper; the resident set becomes a high-water
 *                   mark instead of tracking the live set.
 *   MADV_DONTNEED - the pages are unmapped right away.  Lowest resident
 *                   set, but every recycle costs a TLB shootdown on all
 *                   CPUs.
 * Either way the pages fault back in on the next use. */
void
ui_stack_trim(ui_Goro *g)
{
    int policy = ui_stack_trim_policy;
    if (policy == UI_STACK_TRIM_NONE)
        return;
    ui_stack_trim_range(g, policy == UI_STACK_TRIM_DONTNEED ? MADV_DONTNEED
                                                            : MADV_FREE);
}

/* Unmap the committed pages immediately, regardless of the policy.
 * Used to sweep the resident set of pooled stacks (see
 * UI_STACK_TRIM_SWEEP_INTERVAL). */
void
ui_stack_hard_trim(ui_Goro *g)
{
    ui_stack_trim_range(g, MADV_DONTNEED);
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

    if (fault < base)
    {
        /* One guard page below the base: the committed region already
         * reached the reserve (a doubling grow can fill it in a single
         * step, so the next access lands here instead of on a NONE
         * page inside the stack) — that is a stack overflow.  Faults
         * further below are not stack usage; let the handler re-raise. */
        return (fault >= base - (intptr_t)g->page_size) ? -2 : -1;
    }
    if (fault >= top)
        return -1;

    commit_start = top - (intptr_t)g->stack_committed;
    if (fault >= commit_start)
        return -1;

    /* Minimum needed to cover the faulting page plus one slack page. */
    grow_needed = (size_t)(commit_start - fault) + (size_t)g->page_size;
    grow_needed = ALIGN_UP(grow_needed, (size_t)g->page_size);

    /* Go-style doubling: commit at least twice the current size on
     * every grow, so a deep recursion faults a bounded O(log n) times
     * instead of once per page (each fault costs an mprotect syscall
     * from the SIGSEGV handler).  Capped at the reserve; if even the
     * whole reserve cannot cover the fault, it is a stack overflow. */
    new_committed = g->stack_committed * 2;
    if (new_committed > g->stack_reserve)
        new_committed = g->stack_reserve;

    if (g->stack_committed + grow_needed > new_committed)
        new_committed = g->stack_committed + grow_needed;

    if (new_committed > g->stack_reserve)
        return -2;

    /* mprotect only the newly committed tail: the pages already
     * committed (or warm from a previous life of this arena slot)
     * keep their RW mapping, so reuse stays a cheap same-prot call. */
    void *new_start = (void *)(top - (intptr_t)new_committed);
    void *old_start = (void *)(top - (intptr_t)g->stack_committed);
    if (mprotect(new_start, (size_t)((char *)old_start - (char *)new_start),
                 PROT_READ | PROT_WRITE) < 0)
        return -1;

    g->stack_committed = new_committed;
    return 0;
}

void *
ui_stack_bottom(ui_Goro *g)
{
    return (char *)g->stack_base + g->stack_reserve;
}
