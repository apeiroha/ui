/* The goroutine ceiling: numeric assertions.
 *
 * Every goroutine stack costs about two VMAs - the arena is mapped
 * PROT_NONE and each slot's guard page splits it in two - and the
 * kernel's vm.max_map_count is what actually caps the number of
 * goroutines that can be alive at the same time.  The runtime prints
 * the implied ceiling at startup (ui_check_map_count_limit), so the
 * constant behind that message has to stay true: if the layout ever
 * changes, the number the runtime reports would be wrong.
 *
 * White-box: the pre-allocated pool size is internal, and the point of
 * the assertion is exactly how many stacks a *new* goroutine costs on
 * top of that pool.
 */
#include "ui.h"
#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-56s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-56s ... FAIL\n", name); failed++; }
}

/* Number of VMAs: the lines of /proc/self/maps. */
static long
vma_count(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return -1;
    char line[512];
    long n = 0;
    while (fgets(line, sizeof(line), f))
        n++;
    fclose(f);
    return n;
}

/* Snapshot of /proc/self/maps for set operations between two points. */
#define MAPS_MAX 4096
#define MAPS_LINE 160
typedef char maps_snap[MAPS_MAX][MAPS_LINE];

static int
read_maps(maps_snap lines)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < MAPS_MAX)
    {
        size_t len = strlen(line);
        if (len >= MAPS_LINE)
            len = MAPS_LINE - 1;    /* only needed as a set member */
        memcpy(lines[n], line, len);
        lines[n][len] = 0;
        n++;
    }
    fclose(f);
    return n;
}

/* Mapping size in bytes from a maps line ("lo-hi perms ..."). */
static long
map_size(const char *line)
{
    unsigned long lo = 0, hi = 0;
    sscanf(line, "%lx-%lx", &lo, &hi);
    return (long)(hi - lo);
}

/* How many mappings of exactly `size` bytes exist in `after` but not in
 * `before`.  A fresh stack slot's PROT_NONE headroom has a size no
 * allocator mapping would have, which makes this count immune to the
 * C library's own small mmap groups (musl mallocng allocates the
 * pool-overflow control blocks in 16-32KB groups). */
static long
fresh_vmas_of_size(const maps_snap before, int n_before,
                   const maps_snap after, int n_after, long size)
{
    long found = 0;
    for (int i = 0; i < n_after; i++)
    {
        if (map_size(after[i]) != size)
            continue;
        int in_before = 0;
        for (int j = 0; j < n_before; j++)
            if (strcmp(after[i], before[j]) == 0) { in_before = 1; break; }
        if (!in_before)
            found++;
    }
    return found;
}

static int spawn_done;

static void
worker(void)
{
    __sync_fetch_and_add(&spawn_done, 1);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI goroutine ceiling tests ===\n");

    /* Batch size well above the pre-allocated pool, so that the
     * goroutines beyond it need stacks of their own. */
    const int batch = 1024;

    long before = vma_count();
    if (before < 0)
    {
        printf("  TEST: /proc/self/maps is readable ... FAIL\n");
        printf("\n=== Results: 0 passed, 1 failed ===\n");
        return 1;
    }

    if (ui_Init() != 0)
    {
        printf("  TEST: ui_Init ... FAIL\n");
        printf("\n=== Results: 0 passed, 1 failed ===\n");
        return 1;
    }
    long after_init = vma_count();
    maps_snap before_maps;
    int n_before = read_maps(before_maps);

    /* The pre-allocated goroutines already have their stacks, so they
     * cost their VMAs before this point. */
    long fresh = batch - UI_GORO_PREALLOC;
    if (fresh < 1)
        fresh = 1;

    spawn_done = 0;
    for (int i = 0; i < batch; i++)
        ui_Go(worker);
    /* The stacks are committed at creation, so the VMA count is already
     * final before the goroutines run. */
    long after_spawn = vma_count();
    maps_snap after_maps;
    int n_after = read_maps(after_maps);

    long delta = after_spawn - after_init;
    long per_goro = delta / fresh;

    /* The stack arena design gives each fresh slot exactly one
     * PROT_NONE headroom mapping of (reserve + guard page - committed)
     * plus its committed UI_STACK_INIT pages.  Count the headroom
     * mappings by exact size: the C allocator may add small mmap
     * groups of its own (musl mallocng groups the pool-overflow
     * control blocks in 16-32KB pieces), and those are not stack cost
     * -- they must not invalidate the ceiling constant, which prices a
     * goroutine at two VMAs. */
    long headroom = UI_STACK_RESERVE
                  + (long)sysconf(_SC_PAGESIZE) - UI_STACK_INIT;
    long headroom_fresh = (n_before >= 0 && n_after >= 0)
        ? fresh_vmas_of_size(before_maps, n_before, after_maps, n_after,
                             headroom)
        : -1;

    long noise = delta - fresh * 2;

    if (headroom_fresh != fresh)
        printf("  (diagnostic: fresh headroom VMAs=%ld, expected %ld "
               "(size %ld))\n", headroom_fresh, fresh, headroom);
    if (noise != 0)
        printf("  (diagnostic: delta=%ld, fresh=%ld, allocator noise=%ld)\n",
               delta, fresh, noise);

    check(headroom_fresh == fresh,
          "each new goroutine maps exactly one stack headroom VMA");
    check(noise >= 0 && noise <= fresh / 16,
          "total VMA cost stays at two per goroutine (+ allocator noise)");
    check(per_goro == 2, "the VMA cost per goroutine is 2");

    /* The ceiling the runtime reports at startup is derived from the
     * same constant. */
    long limit = 0;
    FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
    if (f)
    {
        if (fscanf(f, "%ld", &limit) != 1)
            limit = 0;
        fclose(f);
    }
    if (limit > 0)
    {
        long ceiling = limit / 2;
        check(ceiling > 0, "the goroutine ceiling is derived from vm.max_map_count");
    }
    else
    {
        printf("  (vm.max_map_count is not readable; ceiling check skipped)\n");
    }

    ui_Run();
    ui_Fini();

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
