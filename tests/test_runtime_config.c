/* Runtime tuning API: the programmatic counterparts of the environment
 * variables, and the precedence rule between the two.
 *
 *   UI_NVCPUS         -> ui_SetNVCPUs()
 *   UI_YIELD_IO_MASK  -> ui_SetYieldIOMask() / ui_GetYieldIOMask()
 *   UI_IDLE_WAIT_MS   -> ui_SetIdleWaitMs() / ui_GetIdleWaitMs()
 *
 * The rule is: an explicit API call wins over the environment.  It is
 * the same rule the stack policy setters follow, and it is easy to get
 * wrong, so it is asserted here.
 */
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-56s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-56s ... FAIL\n", name); failed++; }
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI runtime config tests ===\n");

    /* ── defaults ── */
    check(ui_GetYieldIOMask() == 255, "default yield-I/O mask is 255");
    check(ui_GetIdleWaitMs() == 100, "default idle wait is 100ms");

    /* ── the env is set to a different value for each, and the API must
     *    still win ── */
    setenv("UI_NVCPUS", "3", 1);
    setenv("UI_YIELD_IO_MASK", "111", 1);
    setenv("UI_IDLE_WAIT_MS", "222", 1);

    /* ── validation ── */
    errno = 0;
    check(ui_SetNVCPUs(0) == -1 && errno == EINVAL,
          "ui_SetNVCPUs rejects 0");
    errno = 0;
    check(ui_SetNVCPUs(1000) == -1 && errno == EINVAL,
          "ui_SetNVCPUs rejects an out-of-range count");
    errno = 0;
    check(ui_SetIdleWaitMs(0) == -1 && errno == EINVAL,
          "ui_SetIdleWaitMs rejects 0");
    check(ui_SetYieldIOMask(0) == 0 && ui_GetYieldIOMask() == 0,
          "ui_SetYieldIOMask accepts 0 (disables the mask)");

    /* ── the API values ── */
    check(ui_SetNVCPUs(2) == 0, "ui_SetNVCPUs accepts 2");
    check(ui_SetYieldIOMask(7) == 0 && ui_GetYieldIOMask() == 7,
          "ui_SetYieldIOMask is read back");
    check(ui_SetIdleWaitMs(50) == 0 && ui_GetIdleWaitMs() == 50,
          "ui_SetIdleWaitMs is read back");

    /* ── the API wins over the environment ── */
    if (ui_Init() != 0)
    {
        printf("  TEST: ui_Init ... FAIL\n");
        return 1;
    }
    check(ui_NVCPUs() == 2, "the API vCPU count overrides UI_NVCPUS");
    check(ui_GetYieldIOMask() == 7,
          "the API yield-I/O mask overrides UI_YIELD_IO_MASK");
    check(ui_GetIdleWaitMs() == 50,
          "the API idle wait overrides UI_IDLE_WAIT_MS");

    /* ── after ui_Init ── */
    errno = 0;
    check(ui_SetNVCPUs(4) == -1 && errno == EBUSY,
          "ui_SetNVCPUs is refused once the vCPUs exist");
    check(ui_SetIdleWaitMs(33) == 0 && ui_GetIdleWaitMs() == 33,
          "the idle wait can still be changed at runtime");

    ui_Fini();

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
