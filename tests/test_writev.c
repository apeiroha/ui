#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int test_passed = 0, test_failed = 0;

#define TEST(n) do { printf("  TEST: %s ... ", n); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); test_passed++; } while (0)
#define FAIL(m) do { printf("FAIL: %s\n", m); test_failed++; } while (0)
#define ASSERT(c, m) do { if (!(c)) { FAIL(m); return; } } while (0)

/* Fallback path: before ui_Init there is no vCPU/ring, so ui_Writev
 * must behave like writev(2). */
static void test_blocking_fallback(void) {
    TEST("ui_Writev fallback (pipe, no UI runtime)");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    char a[] = "hello ";
    char b[] = "writev\n";
    struct iovec iov[2] = {
        { .iov_base = a, .iov_len = 6 },
        { .iov_base = b, .iov_len = 7 },
    };
    ssize_t n = ui_Writev(p[1], iov, 2);
    ASSERT(n == 13, "writev returned 13");

    char buf[32];
    ssize_t got = read(p[0], buf, sizeof(buf));
    ASSERT(got == 13 && memcmp(buf, "hello writev\n", 13) == 0, "payload");
    ASSERT(ui_Writev(p[1], iov, 0) == 0, "iovcnt=0 returns 0");

    close(p[0]);
    close(p[1]);
    PASS();
}

static int pair[2];

static void writer(void) {
    char a[] = "hello ";
    char b[] = "writev\n";
    struct iovec iov[2] = {
        { .iov_base = a, .iov_len = 6 },
        { .iov_base = b, .iov_len = 7 },
    };
    ssize_t n = ui_Writev(pair[0], iov, 2);
    if (n != 13) {
        printf("FAIL: ring writer got %zd (errno %d)\n", n, errno);
        test_failed++;
    }
    shutdown(pair[0], SHUT_WR);
}

static void reader(void) {
    char buf[32];
    size_t got = 0;
    while (got < 13) {
        ssize_t n = ui_Read(pair[1], buf + got, 13 - got);
        if (n <= 0) {
            printf("FAIL: ring reader got %zd\n", n);
            test_failed++;
            return;
        }
        got += (size_t)n;
    }
    if (memcmp(buf, "hello writev\n", 13) != 0) {
        printf("FAIL: ring payload\n");
        test_failed++;
    }
}

static void test_ring_path(void) {
    TEST("ui_Writev io_uring (socketpair, goro)");
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    ASSERT(ui_Init() == 0, "ui_Init");

    ui_Go(writer);
    ui_Go(reader);
    ui_Run();

    close(pair[0]);
    close(pair[1]);
    ui_Fini();
    PASS();
}

int main(void) {
    printf("=== ui_Writev ===\n\n");
    test_blocking_fallback();
    test_ring_path();
    printf("\n=== %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
