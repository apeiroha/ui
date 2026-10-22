/* ui_Run() re-entrancy regression test.
 *
 * ui_Run() used to be one-shot: the vCPUs cleared their `running` flag
 * when the last goroutine retired, and the next ui_Run() started
 * threads that immediately exited, so goroutines spawned between two
 * runs never executed.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-52s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-52s ... FAIL\n", name); failed++; }
}

static int ran;
static void worker(void) { __sync_fetch_and_add(&ran, 1); }

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI run re-entrancy tests ===\n");

    ui_Init();
    ran = 0;

    for (int round = 0; round < 3; round++)
    {
        for (int i = 0; i < 4; i++)
            ui_Go(worker);
        ui_Run();
        check(ran == (round + 1) * 4,
              "goroutines of the current run all executed");
    }

    /* A run with no goroutines must return immediately, not hang. */
    ui_Run();
    check(ran == 12, "an empty run does not disturb the counter");

    /* Spawning after a run and running again must work. */
    ui_Go(worker);
    ui_Run();
    check(ran == 13, "a goroutine spawned after a run still executes");

    ui_Fini();
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
