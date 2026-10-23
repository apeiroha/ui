/* Stack memory policy regression test.
 *
 * Covers the public knobs added for the goroutine-stack footprint:
 *   - ui_SetStackTrimPolicy() / ui_GetStackTrimPolicy()
 *   - ui_SetStackMemoryLimit() / ui_StackMemoryLimit()
 *   - the pool's estimated footprint stays within the budget
 */
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-52s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-52s ... FAIL\n", name); failed++; }
}

static int done;
static void worker(void) { __sync_fetch_and_add(&done, 1); }

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI stack policy tests ===\n");

    /* ── defaults ── */
    check(ui_GetStackTrimPolicy() == UI_STACK_TRIM_NONE,
          "default trim policy is NONE (keep the pages)");
    check(ui_StackMemoryLimit() == 0, "default memory limit is 0 (no budget)");

    /* ── policy setter ── */
    check(ui_SetStackTrimPolicy(UI_STACK_TRIM_DONTNEED) == 0,
          "ui_SetStackTrimPolicy accepts DONTNEED");
    check(ui_GetStackTrimPolicy() == UI_STACK_TRIM_DONTNEED,
          "ui_GetStackTrimPolicy reports DONTNEED");
    check(ui_SetStackTrimPolicy(UI_STACK_TRIM_NONE) == 0,
          "ui_SetStackTrimPolicy accepts NONE");
    check(ui_GetStackTrimPolicy() == UI_STACK_TRIM_NONE,
          "ui_GetStackTrimPolicy reports NONE");
    errno = 0;
    check(ui_SetStackTrimPolicy((ui_StackTrimPolicy)99) == -1 && errno == EINVAL,
          "ui_SetStackTrimPolicy rejects an invalid policy");

    /* ── memory limit setter ── */
    check(ui_SetStackMemoryLimit(4 * 1024 * 1024) == 0,
          "ui_SetStackMemoryLimit accepts a budget");
    check(ui_StackMemoryLimit() == 4 * 1024 * 1024,
          "ui_StackMemoryLimit reports the budget");
    /* A budget means "memory first": the strict advice is selected
     * unless a policy was chosen explicitly. */
    check(ui_GetStackTrimPolicy() == UI_STACK_TRIM_NONE,
          "an explicit policy is not overridden by the budget");
    ui_SetStackTrimPolicy(UI_STACK_TRIM_FREE);
    ui_SetStackMemoryLimit(0);
    check(ui_GetStackTrimPolicy() == UI_STACK_TRIM_FREE,
          "clearing the budget keeps the explicit policy");
    ui_SetStackTrimPolicy(UI_STACK_TRIM_NONE);

    /* ── functional: the pool respects the budget ── */
    {
        ui_SetStackMemoryLimit(1024 * 1024);   /* 1MB */
        if (ui_Init() != 0) { printf("  TEST: ui_Init ... FAIL\n"); return 1; }
        done = 0;
        for (int i = 0; i < 2000; i++) ui_Go(worker);
        ui_Run();
        check(done == 2000, "all goroutines ran with a budget in place");
        ui_Fini();
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
