#include "ui.h"

#include <stdio.h>

/* Regression test for the arena guard page: an unbounded recursion
 * must overflow past the 8MB reserve, fault on the slot's guard page
 * (not silently walk into the adjacent slot's committed region) and
 * terminate with the clean "ui stack overflow: exceeded 8MB limit"
 * report, exit 1 — instead of a raw SIGSEGV (exit 139).
 *
 * The Makefile target checks the exit code and the stderr message.
 *
 * The use of `buf` after the recursive call is load-bearing, not
 * decoration: without it both gcc and clang at -O2 rewrite
 * `recurse(d + 1)` into a sibling call (`jmp recurse`) and drop the
 * 128-byte frame entirely, so the stack never grows, the guard page is
 * never touched, and the goro spins forever — ui_Run() never returns
 * and the test hangs (and drags `make test` down with it).  Keeping a
 * live buffer across the call forces a real `call` with a real frame.
 */

static void
recurse(volatile int d)
{
    volatile char buf[128];
    buf[0] = (char)d;
    recurse(d + 1);
    buf[1] = buf[0];
}

static void
start(void)
{
    recurse(0);
}

int main(void)
{
    if (ui_Init() != 0)
        return 1;
    ui_GoSized(start, 65536);
    ui_Run();
    ui_Fini();
    printf("unexpected return\n");
    return 1;
}
