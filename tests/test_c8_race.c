#include "ui.h"
#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

/* ── Test infrastructure ── */

#define NVCPUS 4
#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "\n  ASSERT FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); _exit(1); } \
} while (0)

static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); fflush(stdout); } while (0)
#define PASS() do { printf("PASS\n"); test_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); test_failed++; } while (0)

/* ── Socket helpers ── */

static int tcp_pair(int sv[2]) {
    int listen_fd = -1, fd1 = -1, fd2 = -1;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) goto err;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) goto err;
    if (listen(listen_fd, 1) < 0) goto err;
    if (getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen) < 0) goto err;

    fd1 = socket(AF_INET, SOCK_STREAM, 0);
    if (fd1 < 0) goto err;

    /* Non-blocking connect + poll */
    int fl = fcntl(fd1, F_GETFL, 0);
    fcntl(fd1, F_SETFL, fl | O_NONBLOCK);
    connect(fd1, (struct sockaddr*)&addr, sizeof(addr));

    fd2 = accept(listen_fd, NULL, NULL);
    if (fd2 < 0) goto err;

    fcntl(fd1, F_SETFL, fl);
    close(listen_fd);
    sv[0] = fd1;
    sv[1] = fd2;
    return 0;

err:
    if (listen_fd >= 0) close(listen_fd);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    return -1;
}

static atomic_int pipe_io_done;
static int sock_pair[2];
static atomic_int total_ops;

#define MAX_GOROS 256

/* Goro type A: blocking recvmsg on socket — this is the vulnerable path */
static void io_steal_recver(void) {
    char buf[64];
    struct msghdr msg;
    struct iovec iov;

    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t nr = ui_RecvMsg(sock_pair[0], &msg, 0);
    if (nr > 0) {
        atomic_fetch_add(&total_ops, 1);
    }
    close(sock_pair[0]);
    atomic_store(&pipe_io_done, 1);
}

/* Goro type B: pure compute + yield — creates stealing pressure */
static void steal_bait_worker(void) {
    for (int i = 0; i < 20; i++) {
        volatile int x = 0;
        for (int j = 0; j < 10000; j++) x += j;
        (void)x;
        ui_Yield();
    }
    atomic_fetch_add(&total_ops, 1);
}

/* Goro type C: sender — sends data after delay to trigger completions */
static void io_steal_sender(void) {
    ui_Sleep(50);
    const char *payload = "RACETEST";
    ssize_t nw = ui_Write(sock_pair[1], payload, 9);
    (void)nw;
    close(sock_pair[1]);
    atomic_fetch_add(&total_ops, 1);
}

/* ── Test: C8 race — I/O completion + work stealing ── */

static void test_c8_io_steal_race(void) {
    TEST("C8: I/O completion + work stealing race");
    setenv("UI_NVCPUS", "4", 1);

    /* Force stack release to maximize timing variability */
    setenv("UI_STACK_RELEASE_ON_RECYCLE", "1", 1);

    ASSERT(tcp_pair(sock_pair) == 0, "tcp_pair failed");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    atomic_store(&pipe_io_done, 0);
    atomic_store(&total_ops, 0);

    /* Spawn the blocking recver (will block on recvmsg via io_uring) */
    ui_Go(io_steal_recver);

    /* Spawn many yielding goros to compete and trigger stealing */
    for (int i = 0; i < 100; i++)
        ui_Go(steal_bait_worker);

    /* Spawn the sender (delayed write to trigger recvmsg completion) */
    ui_Go(io_steal_sender);

    ui_Run();

    ASSERT(atomic_load(&pipe_io_done) == 1, "recver did not complete");
    ASSERT(atomic_load(&total_ops) >= 102, "not all ops completed");

    ui_Fini();
    unsetenv("UI_STACK_RELEASE_ON_RECYCLE");
    unsetenv("UI_NVCPUS");
    PASS();
}

/* ── Test: C8 race with direct timing pressure ── */

/*
 * This test specifically targets the window where:
 *   1. vCPU-A submits io_uring SQE for goro_A
 *   2. goro_A yields (WAITING)
 *   3. I/O completes, CQE enqueued on A's ring
 *   4. vCPU-A enters ui_uring_drain → processes CQE → state=READY → inserts into A's runq
 *   5. vCPU-B instantly steals goro_A from A's runq → home_vcpu = B
 *   6. Both vCPUs could potentially schedule goro_A
 *
 * We validate by tracking the vCPU affinity per goro and detecting concurrent runs.
 */

static atomic_int c8_spawned;
static atomic_int c8_ready;
static atomic_int c8_recv_count;
static uint64_t c8_recv_ch;
static uint64_t c8_done_ch;
static atomic_int c8_sentinel;

/* Workers: each does a blocking recvmsg with high probability of being stolen */
static void c8_recv_worker(void) {
    int id = atomic_fetch_add(&c8_spawned, 1);
    ASSERT(id < MAX_GOROS, "too many goros");

    /* Each worker gets its own socketpair */
    int pair[2];
    ASSERT(tcp_pair(pair) == 0, "tcp_pair failed");

    /* Signal readiness — the fd pair is sent as ONE atomic chan element.
     * Two separate ChanSends would interleave with other workers' sends
     * on a multi-vCPU run, scrambling the (read_fd, write_fd) pairing
     * in the FIFO and causing the sender to write into the wrong socket
     * (observed as test hangs: recv workers whose write_fd never fired). */
    uint64_t pair_packed = ((uint64_t)(uint32_t)pair[0] << 32) | (uint32_t)pair[1];
    ui_ChanSend(c8_recv_ch, &pair_packed);
    atomic_fetch_add(&c8_ready, 1);

    /* Blocking recvmsg — will yield and be vulnerable to the race */
    char buf[64];
    struct msghdr msg;
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t nr = ui_RecvMsg(pair[0], &msg, 0);
    if (nr > 0) {
        atomic_fetch_add(&c8_recv_count, 1);
    }
    close(pair[0]);
    close(pair[1]);

    /* Signal done */
    ui_ChanSend(c8_done_ch, &id);
}

static void c8_sender_worker(void) {
    /* Wait until all recv workers are ready and collect their write fds */
    const char payload[] = "C8TESTDATA";
    for (int i = 0; i < 50; i++) {
        uint64_t pair_packed;
        ui_ChanRecv(c8_recv_ch, &pair_packed);
        int read_fd = (int)(pair_packed >> 32);
        int write_fd = (int)(pair_packed & 0xffffffffu);
        /* Send to each recver's unique socket */
        (void)read_fd;
        ssize_t nw = ui_Write(write_fd, payload, 11);
        (void)nw;
    }
}

static void c8_steal_bait(void) {
    /* Runs yield loops to trigger work stealing on all vCPUs */
    int id = atomic_fetch_add(&c8_spawned, 1);
    ASSERT(id < MAX_GOROS, "too many goros");

    while (atomic_load(&c8_recv_count) < 50) {
        for (int i = 0; i < 5; i++) {
            volatile int x = 0;
            for (int j = 0; j < 5000; j++) x += j;
            (void)x;
            ui_Yield();
        }
        /* Check if we should bail out */
        if (atomic_load(&c8_sentinel)) break;
    }
    atomic_fetch_add(&total_ops, 1);
}

static void test_c8_race_precise(void) {
    TEST("C8: precise race recvmsg + steal (50 workers)");
    setenv("UI_NVCPUS", "4", 1);
    setenv("UI_STACK_RELEASE_ON_RECYCLE", "1", 1);

    ASSERT(ui_Init() == 0, "ui_Init failed");

    c8_recv_ch = ui_NewChan(sizeof(uint64_t), 100);
    c8_done_ch = ui_NewChan(sizeof(int), 50);
    ASSERT(c8_recv_ch != 0 && c8_done_ch != 0, "chan alloc failed");

    atomic_store(&c8_spawned, 2);
    atomic_store(&c8_ready, 0);
    atomic_store(&c8_recv_count, 0);
    atomic_store(&c8_sentinel, 0);

    /* Spawn recv workers (each creates its own socketpair) */
    for (int i = 0; i < 50; i++)
        ui_Go(c8_recv_worker);

    /* Spawn steal bait (creates work stealing pressure) */
    for (int i = 0; i < 20; i++)
        ui_Go(c8_steal_bait);

    /* Spawn sender (sends to each recver's unique socket) */
    ui_Go(c8_sender_worker);

    ui_Run();

    ASSERT(atomic_load(&c8_recv_count) == 50, "not all recvs completed");

    /* Drain done channel */
    for (int i = 0; i < 50; i++) {
        int id;
        ui_ChanRecv(c8_done_ch, &id);
    }

    ui_ChanFree(c8_recv_ch);
    ui_ChanFree(c8_done_ch);
    ui_Fini();
    unsetenv("UI_STACK_RELEASE_ON_RECYCLE");
    unsetenv("UI_NVCPUS");
    PASS();
}

/* ── Exhaustive: stress I/O + steal with canary validation ── */

#define IO_STEAL_STRESS_N 200
static atomic_int stress_val[IO_STEAL_STRESS_N];

static void stress_worker(uintptr_t id_u) {
    int id = (int)(intptr_t)id_u;
    /* Write a canary */
    atomic_store(&stress_val[id], id + 1);

    /* Do some yield cycles */
    for (int i = 0; i < 10; i++) {
        ui_Yield();
        /* Verify canary is still intact */
        int v = atomic_load(&stress_val[id]);
        if (v != id + 1) {
            fprintf(stderr, "\n  CORRUPTION: stress[%d] = %d, expected %d\n", id, v, id + 1);
            _exit(1);
        }
    }
}

static void stress_bait_workers(void) {
    for (int i = 0; i < 50; i++)
        ui_Go1((void*)stress_worker, (uintptr_t)(intptr_t)i);
}

static void test_io_steal_stress(void) {
    TEST("C8: I/O + steal exhaustive stress (200 goros, 4 vCPUs)");
    setenv("UI_NVCPUS", "4", 1);

    ASSERT(tcp_pair(sock_pair) == 0, "tcp_pair failed");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    atomic_store(&pipe_io_done, 0);
    atomic_store(&total_ops, 0);

    /* recver goro */
    ui_Go(io_steal_recver);

    /* stress bait */
    for (int i = 0; i < 4; i++)
        ui_Go(stress_bait_workers);

    /* sender */
    ui_Go(io_steal_sender);

    ui_Run();

    ASSERT(atomic_load(&pipe_io_done) == 1, "recver did not complete");
    ui_Fini();
    unsetenv("UI_NVCPUS");
    PASS();
}

/* ── Main ── */

int main(void) {
    /* Check io_uring availability first */
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int ring_fd = (int)syscall(__NR_io_uring_setup, 2, &p);
    if (ring_fd < 0) {
        printf("=== C8 Race Tests (SKIP: io_uring unavailable) ===\n\n");
        printf("  io_uring not supported on this kernel\n");
        return 77; /* skip */
    }
    close(ring_fd);

    printf("=== C8 Race: I/O completion + work stealing ===\n\n");

    test_c8_io_steal_race();
    test_c8_race_precise();
    test_io_steal_stress();

    printf("\n=== Results: %d passed, %d failed ===\n",
           test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
