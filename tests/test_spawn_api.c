/* Spawn API regression tests.
 *
 * Covers the failure semantics of the spawn family:
 *   - ui_Go() and friends abort the process when a goroutine cannot be
 *     created (a silently dropped task is a correctness bug),
 *   - ui_TryGo()/ui_TryGo1() report the failure instead.
 *
 * The failure is triggered with the UI_DEBUG_GORO_LIMIT hook, so the
 * test does not have to exhaust the address space to reach it.
 */
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef UI_DEBUG_GORO_LIMIT
extern int ui_debug_goro_limit;
#else
#error "build this test with -DUI_DEBUG_GORO_LIMIT"
#endif

static int passed, failed;

static void
check(int ok, const char *name)
{
    if (ok) { printf("  TEST: %-52s ... PASS\n", name); passed++; }
    else    { printf("  TEST: %-52s ... FAIL\n", name); failed++; }
}

static int ran;
static void worker(void) { ran++; }

static void fn1(void *arg) { (void)arg; ran++; }

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI spawn API tests ===\n");

    /* ── 1. ui_Go / ui_TryGo success path ── */
    {
        ui_Init();
        ran = 0;
        uint64_t id = 0;
        check(ui_TryGo(worker, &id) == 0 && id != 0, "ui_TryGo returns 0 and a goroutine id");
        check(ui_Go(worker) != 0, "ui_Go returns a non-zero goroutine id");
        check(ui_TryGo1((void *)fn1, 0, &id) == 0 && id != 0, "ui_TryGo1 returns 0 and a goroutine id");
        ui_Run();
        check(ran == 3, "all spawned goroutines ran");
        ui_Fini();
    }

    /* ── 2. ui_TryGo reports exhaustion ── */
    {
        ui_Init();
        ran = 0;
        ui_debug_goro_limit = 2;      /* two allocations are allowed */
        uint64_t id = 0;
        check(ui_TryGo(worker, &id) == 0, "ui_TryGo succeeds below the limit");
        check(ui_TryGo(worker, &id) == 0, "ui_TryGo succeeds at the limit");
        errno = 0;
        check(ui_TryGo(worker, &id) == -1, "ui_TryGo fails past the limit");
        check(errno == ENOMEM, "ui_TryGo sets errno to ENOMEM");
        check(ui_TryGo1((void *)fn1, 0, &id) == -1, "ui_TryGo1 fails past the limit");
        ui_debug_goro_limit = 0;
        ui_Run();
        check(ran == 2, "only the goroutines that were accepted ran");
        ui_Fini();
    }

    /* ── 3. ui_Go aborts instead of dropping the task ── */
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            /* Child: the limit is hit, so ui_Go must abort. */
            freopen("/dev/null", "w", stderr);
            ui_Init();
            ui_debug_goro_limit = 1;
            ui_Go(worker);          /* must not return */
            ui_Go(worker);          /* must not return */
            _exit(0);               /* reached only if ui_Go did not abort */
        }
        else if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
            check(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
                  "ui_Go aborts the process when a goroutine cannot be created");
        }
        else
        {
            check(0, "fork failed");
        }
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
