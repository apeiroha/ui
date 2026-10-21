/* Repeated ui_Init/ui_Fini + channel stress.
 *
 * This is the r11c mode-2 shape: a fresh runtime each round, four senders and
 * four receivers on a small buffered channel.  It used to crash (SIGSEGV in
 * ui_steal_work / corrupted goro stacks) or hang within a few hundred rounds.
 * The Makefile watchdog turns a hang into a test failure.
 */
#include "ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define NSEND 4
#define NRECV 4
#define OPS   2000
#define ROUNDS 300

static uint64_t ch;
static atomic_int finished;

static void
sender(void)
{
    int64_t v = 42;
    for (int i = 0; i < OPS; i++)
        ui_ChanSend(ch, &v);
    atomic_fetch_add(&finished, 1);
}

static void
receiver(void)
{
    int64_t v;
    for (int i = 0; i < OPS; i++)
        ui_ChanRecv(ch, &v);
    atomic_fetch_add(&finished, 1);
}

int
main(void)
{
    setenv("UI_NVCPUS", "4", 1);
    for (int round = 1; round <= ROUNDS; round++)
    {
        if (ui_Init() != 0)
        {
            puts("INIT_FAILED");
            return 2;
        }
        atomic_store(&finished, 0);
        ch = ui_NewChan(sizeof(int64_t), 16);
        if (!ch)
        {
            puts("CHAN_FAILED");
            return 2;
        }
        for (int i = 0; i < NSEND; i++)
            ui_Go(sender);
        for (int i = 0; i < NRECV; i++)
            ui_Go(receiver);
        ui_Run();
        if (atomic_load(&finished) != NSEND + NRECV)
        {
            printf("round %d: only %d/%d goros finished\n",
                   round, atomic_load(&finished), NSEND + NRECV);
            return 1;
        }
        ui_ChanFree(ch);
        ui_Fini();
        if (round % 50 == 0)
        {
            printf("round %d ok\n", round);
            fflush(stdout);
        }
    }
    puts("cycle stress: all rounds completed");
    return 0;
}
