#include "ui.h"

#include <stdio.h>

/* Regression test for the arena guard page: an unbounded recursion
 * must overflow past the 8MB reserve, fault on the slot's guard page
 * (not silently walk into the adjacent slot's committed region) and
 * terminate with the clean "ui stack overflow: exceeded 8MB limit"
 * report, exit 1 — instead of a raw SIGSEGV (exit 139).
 *
 * The Makefile target checks the exit code and the stderr message. */

static void
recurse(volatile int d)
{
    volatile char buf[128];
    (void)buf;
    recurse(d + 1);
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
