#define _GNU_SOURCE
#include "ui.h"
#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

static int test_passed = 0, test_failed = 0;

#define TEST(n) do { printf("  TEST: %s ... ", n); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); test_passed++; } while (0)
#define FAIL(m) do { printf("FAIL: %s\n", m); test_failed++; } while (0)
#define ASSERT(c, m) do { if (!(c)) { FAIL(m); return; } } while (0)

/* Producer-consumer via UDP: server echoes, client sends+validates.
 * Both use ui_RecvMsg/ui_SendMsg on io_uring — this tests the async
 * I/O path across goros on different vCPUs.
 * No explicit shutdown needed: server exits after echoing N msgs,
 * client exits after receiving N echoes, ui_Run returns. */

#define NMSGS 200
#define NWORKERS 8
static atomic_int echo_count;
static atomic_int running;
static uint64_t ch_port;

static void worker(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[1500];
    struct sockaddr_storage peer;
    struct iovec iov;
    struct msghdr msg;

    while (atomic_load(&running)) {
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_flags = 0;

        /* Non-blocking recv: if no data, yield and retry */
        ssize_t n = ui_RecvMsg(fd, &msg, MSG_DONTWAIT);
        if (n == -EAGAIN) { ui_Yield(); continue; }
        if (n <= 0) break;

        /* Echo back (blocking — send always completes quickly
         * on a local socket) */
        iov.iov_base = buf;
        iov.iov_len = (size_t)n;
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        ui_SendMsg(fd, &msg, 0);

        atomic_fetch_add(&echo_count, 1);
    }
}

static void server(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT(fd >= 0, "server socket");

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    ASSERT(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");

    socklen_t sl = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &sl);
    int port = ntohs(addr.sin_port);

    atomic_store(&running, 1);
    ui_ChanSend(ch_port, &port);

    for (int i = 0; i < NWORKERS; i++)
        ui_Go1(worker, (uintptr_t)(intptr_t)fd);

    /* Poll until enough echoes received, then shutdown */
    while (atomic_load(&echo_count) < NMSGS)
        ui_Yield();

    atomic_store(&running, 0);
    close(fd);
}

static void client(void) {
    int port;
    ui_ChanRecv(ch_port, &port);
    ASSERT(port > 0, "got port");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT(fd >= 0, "client socket");

    struct sockaddr_in srv = { .sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons((uint16_t)port) };
    ASSERT(connect(fd, (struct sockaddr*)&srv, sizeof(srv)) == 0, "connect");

    char buf[1500];
    struct iovec iov;
    struct msghdr msg;

    for (int seq = 0; seq < NMSGS; seq++) {
        int *h = (int *)buf;
        h[0] = seq;
        h[1] = 0xCAFEBABE;
        size_t dlen = 64;

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf;
        iov.iov_len = dlen;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        ssize_t ns = ui_SendMsg(fd, &msg, 0);
        if (ns != (ssize_t)dlen) break;

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        ssize_t nr = ui_RecvMsg(fd, &msg, 0);
        if (nr != (ssize_t)dlen) break;
        int *rh = (int *)buf;
        if (rh[0] != seq || rh[1] != (int)0xCAFEBABE) break;
    }
    close(fd);
}

static void test_udp_pingpong(void) {
    TEST("UDP ping-pong (200 msgs, io_uring, 4 vCPUs)");
    struct io_uring_params p = {0};
    int rfd = syscall(__NR_io_uring_setup, 2, &p);
    if (rfd < 0) { FAIL("io_uring N/A"); return; }
    close(rfd);

    setenv("UI_NVCPUS", "4", 1);
    ASSERT(ui_Init() == 0, "ui_Init");
    ch_port = ui_NewChan(sizeof(int), 1);
    ASSERT(ch_port, "chan");

    atomic_store(&echo_count, 0);
    atomic_store(&running, 0);
    ui_Go(server);
    ui_Go(client);
    ui_Run();
    ASSERT(atomic_load(&echo_count) >= NMSGS, "echo count");

    ui_ChanFree(ch_port);
    ui_Fini();
    unsetenv("UI_NVCPUS");
    PASS();
}

static void test_udp_workers(void) {
    TEST("UDP multi-worker (8 workers, 200 msgs, io_uring, 4 vCPUs)");
    struct io_uring_params p = {0};
    int rfd = syscall(__NR_io_uring_setup, 2, &p);
    if (rfd < 0) { FAIL("io_uring N/A"); return; }
    close(rfd);

    setenv("UI_NVCPUS", "4", 1);
    ASSERT(ui_Init() == 0, "ui_Init");
    ch_port = ui_NewChan(sizeof(int), 1);
    ASSERT(ch_port, "chan");

    atomic_store(&echo_count, 0);
    atomic_store(&running, 0);
    ui_Go(server);
    ui_Go(client);
    ui_Run();
    ASSERT(atomic_load(&echo_count) >= NMSGS, "echo count");

    ui_ChanFree(ch_port);
    ui_Fini();
    unsetenv("UI_NVCPUS");
    PASS();
}

int main(void) {
    printf("=== UDP concurrent echo ===\n\n");
    test_udp_pingpong();
    test_udp_workers();
    printf("\n=== %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
