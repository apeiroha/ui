#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <fcntl.h>

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

#define REPORT(op, us, count) do { \
    double ns = (double)(us) * 1000.0 / (count); \
    printf("%8.0f ns/op  (%d ops in %lluus)\n", ns, (int)(count), (unsigned long long)(us)); \
} while(0)

static int io_pair[2];
static int io_target;
static int io_count;
static char io_buf[4096];

static void io_reader(void) {
    int count = 0;
    while (count < io_target) {
        ssize_t n = ui_Read(io_pair[0], io_buf, 512);
        if (n <= 0) break;
        count++;
    }
    io_count = count;
}

static void bench_io_fastpath(int N) {
    printf("\n--- I/O fast path (%d ops) ---\n", N);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, io_pair) < 0) {
        perror("socketpair"); return;
    }
    int bufsz = 16*1024*1024;  /* kernel doubles → 32MB effective */
    setsockopt(io_pair[0], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(io_pair[1], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    /* Prefill: write as many chunks as the buffer can hold (non-blocking) */
    int prefill_count = 0;
    int flags = fcntl(io_pair[1], F_GETFL);
    fcntl(io_pair[1], F_SETFL, flags | O_NONBLOCK);
    char *prebuf = malloc(512);
    memset(prebuf, 'A', 512);
    for (int i = 0; i < N + 4096; i++) {
        int n = (int)write(io_pair[1], prebuf, 512);
        if (n <= 0) break;  /* EAGAIN or error */
        prefill_count++;
    }
    fcntl(io_pair[1], F_SETFL, flags);  /* restore blocking */
    free(prebuf);

    if (prefill_count < N) {
        printf("  WARNING: only pre-filled %d chunks (need %d)\n", prefill_count, N);
        printf("  Using prefill_count=%d as N\n", prefill_count);
        N = prefill_count;
    }

    ui_Init();
    io_target = N;
    io_count = 0;

    uint64_t start = now_us();
    ui_Go(io_reader);
    ui_Run();
    uint64_t end = now_us();

    printf("  %-45s ", "ui_Read (always-available data)");
    REPORT("read", end - start, io_count);

    ui_Fini();
    close(io_pair[0]); close(io_pair[1]);
}

int main(void) {
    printf("UI I/O benchmarks\n");
    printf("=================\n");

    bench_io_fastpath(100000);

    printf("\nDone.\n");
    return 0;
}
