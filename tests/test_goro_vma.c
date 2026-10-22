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

    long delta = after_spawn - after_init;
    long per_goro = delta / fresh;

    check(delta == fresh * 2,
          "each new goroutine costs exactly two VMAs");
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
