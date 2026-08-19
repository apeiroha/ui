#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/socket.h>

/* Regression test for the "I/O completion raced its switch" drop:
 * a goro parks for blocking I/O (state=WAITING) and its completion is
 * reaped by the scheduler drain of the SAME schedule pass, while
 * v->current still points at the just-parked goro.  If the drain takes
 * the "current" branch (state=READY without enqueue) and the schedule
 * refuses to re-insert non-voluntary goros (fcc4739's voluntary-only
 * re-insert), the goro is never run again: io_pending is already 0 and
 * nothing ever wakes it.  The fix detaches v->current BEFORE the
 * drains so the scheduler-context drain delivers the parked goro via
 * the normal enqueue path.
 *
 * Lockstep design: the worker loops blocking recv()s; an OS writer
 * thread writes one byte per consumed recv.  Each write races the
 * worker's next submit→park window, so a large fraction of completions
 * land inside the window where the bug fires: the worker gets dropped,
 * the ack stream stops, and the watchdog fails the test. */

#define ITERS 5000
#define ACK_TIMEOUT_MS 15000

static int pair[2];
static atomic_int consumed;
static atomic_int aborted;

static void *
writer_thread(void *arg)
{
    (void)arg;
    char b = 'x';
    for (int i = 0; i < ITERS; i++)
    {
        /* Write first, then wait for the worker to consume it. */
        if (write(pair[1], &b, 1) != 1) { perror("write"); _exit(1); }
        while (atomic_load_explicit(&consumed, memory_order_acquire) < i + 1)
        {
            if (atomic_load_explicit(&aborted, memory_order_relaxed))
                return NULL;
        }
    }
    return NULL;
}

static void
worker_recv(void)
{
    char buf;
    for (int i = 0; i < ITERS; i++)
    {
        ssize_t n = ui_Recv(pair[0], &buf, 1, 0);
        if (n != 1)
        {
            fprintf(stderr, "\n  FAIL: recv returned %zd\n", n);
            _exit(1);
        }
        atomic_fetch_add_explicit(&consumed, 1, memory_order_release);
    }
}

static void
worker_recvbatch(void)
{
    char buf;
    struct mmsghdr msg;
    struct iovec iov;
    iov.iov_base = &buf;
    iov.iov_len = sizeof(buf);
    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.msg_iov = &iov;
    msg.msg_hdr.msg_iovlen = 1;

    for (int i = 0; i < ITERS; i++)
    {
        int n = ui_RecvBatch(pair[0], &msg, 1, 0);
        if (n != 1)
        {
            fprintf(stderr, "\n  FAIL: RecvBatch returned %d\n", n);
            _exit(1);
        }
        atomic_fetch_add_explicit(&consumed, 1, memory_order_release);
    }
}

static void
watchdog(void)
{
    for (int i = 0; i < ACK_TIMEOUT_MS / 100; i++)
    {
        ui_Sleep(100);
        if (atomic_load_explicit(&consumed, memory_order_relaxed) >= ITERS)
            return;
    }
    fprintf(stderr,
            "\n  FAIL: worker stalled at %d/%d recvs — I/O completion "
            "dropped in the parking-pass drain\n",
            atomic_load_explicit(&consumed, memory_order_relaxed), ITERS);
    atomic_store(&aborted, 1);
    _exit(1);
}

static int
run_scenario(void (*worker)(void), const char *name)
{
    setenv("UI_NVCPUS", "1", 1);
    if (ui_Init() != 0) { fprintf(stderr, "ui_Init failed\n"); return 1; }

    atomic_store(&consumed, 0);
    atomic_store(&aborted, 0);
    pthread_t tw;
    if (pthread_create(&tw, NULL, writer_thread, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
    ui_Go(worker);
    ui_Go(watchdog);
    ui_Run();
    pthread_join(tw, NULL);

    ui_Fini();
    unsetenv("UI_NVCPUS");

    if (atomic_load(&consumed) != ITERS)
    {
        printf("  TEST: %s ... FAIL\n", name);
        return 1;
    }
    printf("  TEST: %s ... PASS\n", name);
    return 0;
}

int main(void)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    {
        perror("socketpair");
        return 1;
    }

    printf("=== UI: I/O park vs parking-pass drain race ===\n\n");

    int failed = 0;
    failed |= run_scenario(worker_recv, "lockstep ui_Recv, 1 vCPU");
    failed |= run_scenario(worker_recvbatch, "lockstep ui_RecvBatch, 1 vCPU");

    printf("\n=== Results: %s ===\n", failed ? "FAILED" : "2 passed, 0 failed");
    return failed ? 1 : 0;
}
