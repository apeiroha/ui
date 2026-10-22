/* UI (うい) I/O benchmarks.
 *
 * Modeled on Go's testing.B (see tests/bench.h).  The metric name is
 * the one the dashboard already tracks - do not rename it without also
 * updating web/src/metrics.test.ts.
 */
#include "ui.h"
#include "bench.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int    io_pair[2];
static int    io_target;
static int    io_count;
static char   io_buf[4096];

/* Read `io_target` chunks of 512 bytes out of the socket. */
static void
io_reader(void)
{
    int count = 0;
    while (count < io_target)
    {
        ssize_t n = ui_Read(io_pair[0], io_buf, 512);
        if (n <= 0) break;
        count++;
    }
    io_count = count;
}

/* The data is already in the socket buffer, so every read completes
 * without parking.  The socket buffer bounds how much can be pre-filled,
 * so the real op count can be smaller than requested - it is reported
 * so the number is not silently wrong.
 *
 * The op count is fixed rather than auto-scaled: the amount of data
 * that fits in the socket buffer does not grow with the iteration
 * count, so scaling it would only change the clamp.
 */
static void
bench_io_fastpath(ui_bench_ctx *b, int n)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, io_pair) < 0)
    {
        perror("socketpair");
        ui_bench_set_ops(b, 0);
        return;
    }
    int bufsz = 16 * 1024 * 1024;  /* kernel doubles -> 32MB effective */
    setsockopt(io_pair[0], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(io_pair[1], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    /* Prefill: write as many chunks as the buffer can hold (non-blocking) */
    int prefill_count = 0;
    int flags = fcntl(io_pair[1], F_GETFL);
    fcntl(io_pair[1], F_SETFL, flags | O_NONBLOCK);
    char *prebuf = malloc(512);
    if (!prebuf)
    {
        ui_bench_set_ops(b, 0);
        return;
    }
    memset(prebuf, 'A', 512);
    for (int i = 0; i < n + 4096; i++)
    {
        int w = (int)write(io_pair[1], prebuf, 512);
        if (w <= 0) break;  /* EAGAIN or error */
        prefill_count++;
    }
    fcntl(io_pair[1], F_SETFL, flags);  /* restore blocking */
    free(prebuf);

    ui_Init();
    io_target = prefill_count;
    io_count = 0;

    ui_bench_start(b);
    ui_Go(io_reader);
    ui_Run();
    ui_bench_stop(b);

    ui_bench_set_ops(b, io_count);
    ui_bench_report_extra(b, "prefilled", (double)prefill_count, "chunks");

    ui_Fini();
    close(io_pair[0]);
    close(io_pair[1]);
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("UI I/O benchmarks\n");
    printf("=================\n\n");

    ui_bench_add("bench_ui_io", "ui_Read (always-available data)",
                 bench_io_fastpath, NULL, 100000);

    int rc = ui_bench_main(argc, argv);
    printf("\nDone.\n");
    return rc;
}
