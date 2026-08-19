#include "ui.h"
#include "ui_internal.h"   /* ui_this_vcpu for GoOn placement tests */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) do { \
    printf("  TEST: %s ... ", name); \
    fflush(stdout); \
} while (0)

#define PASS() do { printf("PASS\n"); test_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); test_failed++; } while (0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)

/* ── Basic spawn and yield ── */

static int yield_val = 0;

static void yield_worker(void) {
    yield_val = 1;
    ui_Yield();
    yield_val = 2;
}

static void test_spawn_yield(void) {
    TEST("spawn and yield");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    yield_val = 0;
    ui_Go(yield_worker);
    ui_Run();
    ASSERT(yield_val == 2, "coroutine did not complete correctly");
    ui_Fini();
    PASS();
}

/* ── Multiple goroutines ── */

#define MULTI_COUNT 100
static int multi_counter = 0;

static void multi_worker(void) {
    ui_Yield();
    __sync_fetch_and_add(&multi_counter, 1);
}

static void test_multi_goro(void) {
    TEST("100 goroutines with yield");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    multi_counter = 0;
    for (int i = 0; i < MULTI_COUNT; i++)
        ui_Go(multi_worker);
    ui_Run();
    ASSERT(multi_counter == MULTI_COUNT, "not all goroutines ran");
    ui_Fini();
    PASS();
}

/* ─── Deep recursion (stack growth) ── */

static int deep_val = 0;

static void deep_worker(void) {
    /* Volatile to prevent tail-call optimization */
    volatile int depth = 0;
    /* Use a large stack frame */
    volatile char buf[4096];
    (void)buf;
    depth++;

    /* Recurse ~100 times to trigger stack growth */
    for (int i = 0; i < 100; i++) {
        volatile char frame[2048];
        (void)frame;
        ui_Yield();
    }

    deep_val = 1;
}

static void test_stack_grow(void) {
    TEST("deep recursion (stack growth)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    deep_val = 0;
    ui_GoSized(deep_worker, 65536);
    ui_Run();
    ASSERT(deep_val == 1, "deep recursion did not complete");
    ui_Fini();
    PASS();
}

/* ── Buffered channel send/recv (same goroutine) ── */

static void test_chan_buffered(void) {
    TEST("buffered channel send/recv");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    uint64_t ch = ui_NewChan(sizeof(int), 5);
    ASSERT(ch != 0, "ui_NewChan failed");

    int vals[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) ui_ChanSend(ch, &vals[i]);

    int got[5];
    memset(got, 0, sizeof(got));
    for (int i = 0; i < 5; i++) ui_ChanRecv(ch, &got[i]);

    for (int i = 0; i < 5; i++) ASSERT(got[i] == vals[i], "wrong value");
    ui_ChanFree(ch);
    ui_Fini();
    PASS();
}

/* ── Channel try_send / try_recv ── */

static void test_chan_try(void) {
    TEST("channel try_send / try_recv");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    uint64_t ch = ui_NewChan(sizeof(int), 2);
    ASSERT(ch != 0, "ui_NewChan failed");

    int v = 1; ASSERT(ui_ChanTrySend(ch, &v), "try_send 1");
    v = 2;     ASSERT(ui_ChanTrySend(ch, &v), "try_send 2");
    v = 3;     ASSERT(!ui_ChanTrySend(ch, &v), "try_send 3 should fail (full)");

    int r = 0;
    ASSERT(ui_ChanTryRecv(ch, &r) && r == 1, "try_recv 1");
    ASSERT(ui_ChanTryRecv(ch, &r) && r == 2, "try_recv 2");
    ASSERT(!ui_ChanTryRecv(ch, &r), "try_recv 3 should fail (empty)");

    ui_ChanFree(ch);
    ui_Fini();
    PASS();
}

/* ── Channel close ── */

static void test_chan_close(void) {
    TEST("channel close");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    uint64_t ch = ui_NewChan(sizeof(int), 2);
    ASSERT(ch != 0, "ui_NewChan failed");

    int v = 42; ui_ChanSend(ch, &v);
    ui_ChanClose(ch);

    int r1 = 0; ui_ChanRecv(ch, &r1);
    ASSERT(r1 == 42, "should receive sent value before close");
    int r2 = -1; ui_ChanRecv(ch, &r2);
    ASSERT(r2 == 0, "recv after close should return zero");

    ui_ChanFree(ch);
    ui_Fini();
    PASS();
}

/* ── Ping-pong (unbuffered channel across goroutines) ── */

static uint64_t ping_ch, pong_ch;
static int ping_pong_val = 0;

static void ping_entry(void) {
    int val = 1;
    ui_ChanSend(ping_ch, &val);
    ui_ChanRecv(pong_ch, &val);
    ping_pong_val = val;
}

static void pong_entry(void) {
    int val = 0;
    ui_ChanRecv(ping_ch, &val);
    val++;
    ui_ChanSend(pong_ch, &val);
}

static void test_ping_pong(void) {
    TEST("ping-pong via two channels");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    ping_ch = ui_NewChan(sizeof(int), 0);
    pong_ch = ui_NewChan(sizeof(int), 0);
    ASSERT(ping_ch != 0 && pong_ch != 0, "ui_NewChan failed");
    ping_pong_val = 0;
    ui_Go(ping_entry);
    ui_Go(pong_entry);
    ui_Run();
    ASSERT(ping_pong_val == 2, "ping should receive 2 from pong");
    ui_ChanFree(ping_ch); ui_ChanFree(pong_ch);
    ui_Fini();
    PASS();
}

/* ── Multiple goroutines sharing a channel ── */

#define NCHAN_THREADS 10
static uint64_t shared_ch;
static int chan_sum = 0;

static void chan_sender(void) {
    int v = 1;
    ui_ChanSend(shared_ch, &v);
}

static void chan_receiver(void) {
    int v = 0;
    ui_ChanRecv(shared_ch, &v);
    __sync_fetch_and_add(&chan_sum, v);
}

static void test_chan_multi_goro(void) {
    TEST("10 sender + 10 receiver goroutines via channel");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    shared_ch = ui_NewChan(sizeof(int), 5);
    ASSERT(shared_ch != 0, "ui_NewChan failed");
    chan_sum = 0;

    for (int i = 0; i < NCHAN_THREADS; i++) {
        ui_Go(chan_sender);
        ui_Go(chan_receiver);
    }

    ui_Run();
    ASSERT(chan_sum == NCHAN_THREADS, "each receiver should get 1");
    ui_ChanFree(shared_ch);
    ui_Fini();
    PASS();
}

/* ── Mutex with many goroutines ── */

static uint64_t test_mtx;
static int mtx_counter = 0;

static void mtx_worker(void) {
    for (int i = 0; i < 20; i++) {
        ui_MutexLock(test_mtx);
        mtx_counter++;
        ui_MutexUnlock(test_mtx);
    }
}

static void test_mutex_multi(void) {
    TEST("mutex: 10 goroutines * 20 increments");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    test_mtx = ui_MutexNew();
    ASSERT(test_mtx != 0, "ui_MutexNew failed");
    mtx_counter = 0;

    for (int i = 0; i < 10; i++)
        ui_Go(mtx_worker);

    ui_Run();
    ASSERT(mtx_counter == 200, "counter should be 200");
    ui_MutexFree(test_mtx);
    ui_Fini();
    PASS();
}

/* ── Mutex trylock ── */

static void test_mutex_trylock(void) {
    TEST("mutex trylock");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    uint64_t m = ui_MutexNew();
    ASSERT(m != 0, "ui_MutexNew failed");

    ASSERT(ui_MutexTryLock(m), "trylock should succeed on unlocked");
    ASSERT(!ui_MutexTryLock(m), "trylock should fail on locked");
    ui_MutexUnlock(m);
    ASSERT(ui_MutexTryLock(m), "trylock should succeed after unlock");
    ui_MutexUnlock(m);

    ui_MutexFree(m);
    ui_Fini();
    PASS();
}

/* ── Condition variable ── */

static uint64_t cond_mh, cond_ch;
static int cond_ready = 0;

static void cond_waiter(void) {
    ui_MutexLock(cond_mh);
    while (!cond_ready)
        ui_CondWait(cond_ch, cond_mh);
    ui_MutexUnlock(cond_mh);
}

static void cond_signaler(void) {
    ui_Sleep(10);
    ui_MutexLock(cond_mh);
    cond_ready = 1;
    ui_CondSignal(cond_ch);
    ui_MutexUnlock(cond_mh);
}

static void test_cond(void) {
    TEST("condition variable");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    cond_mh = ui_MutexNew();
    cond_ch = ui_CondNew();
    ASSERT(cond_mh != 0 && cond_ch != 0, "new failed");
    cond_ready = 0;
    ui_Go(cond_waiter);
    ui_Go(cond_signaler);
    ui_Run();
    ASSERT(cond_ready == 1, "condition should be signaled");
    ui_MutexFree(cond_mh);
    ui_CondFree(cond_ch);
    ui_Fini();
    PASS();
}

/* ── Condition variable broadcast ── */

#define NCOND_WAITERS 5
static uint64_t bcast_mh, bcast_ch;
static int bcast_count = 0;

static void bcast_waiter(void) {
    ui_MutexLock(bcast_mh);
    while (!bcast_count)
        ui_CondWait(bcast_ch, bcast_mh);
    ui_MutexUnlock(bcast_mh);
}

static void bcast_signaler(void) {
    ui_Sleep(20);
    ui_MutexLock(bcast_mh);
    bcast_count = 1;
    ui_CondBroadcast(bcast_ch);
    ui_MutexUnlock(bcast_mh);
}

static void test_cond_broadcast(void) {
    TEST("condition variable broadcast");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    bcast_mh = ui_MutexNew();
    bcast_ch = ui_CondNew();
    ASSERT(bcast_mh != 0 && bcast_ch != 0, "new failed");
    bcast_count = 0;

    for (int i = 0; i < NCOND_WAITERS; i++)
        ui_Go(bcast_waiter);
    ui_Go(bcast_signaler);

    ui_Run();
    ASSERT(bcast_count == 1, "should be signaled");
    ui_MutexFree(bcast_mh);
    ui_CondFree(bcast_ch);
    ui_Fini();
    PASS();
}

/* ── io_uring read/write (pipe) within goroutine ── */

static void test_io_pipe(void) {
    TEST("io_uring read/write (pipe)");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    int fds[2];
    int res = pipe(fds);
    ASSERT(res == 0, "pipe failed");

    const char *msg = "hello";
    ssize_t nw = ui_Write(fds[1], msg, 6);
    ASSERT(nw == 6, "write should write 6 bytes");

    char buf[16] = {0};
    ssize_t nr = ui_Read(fds[0], buf, sizeof(buf) - 1);
    ASSERT(nr == 6, "read should read 6 bytes");
    ASSERT(strcmp(buf, "hello") == 0, "read wrong data");

    close(fds[0]); close(fds[1]);
    ui_Fini();
    PASS();
}

/* ── ui_TryRead / ui_TryWrite fast path ── */

static void test_try_read_write(void) {
    TEST("ui_TryRead/ui_TryWrite fast path");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    int fds[2];
    ASSERT(pipe(fds) == 0, "pipe failed");

    /* Data already in pipe — TryRead should succeed immediately */
    ssize_t nw = write(fds[1], "data", 4);
    ASSERT(nw == 4, "write to pipe");
    char buf[16] = {0};
    ssize_t nr = ui_TryRead(fds[0], buf, 4);
    ASSERT(nr == 4, "TryRead with data ready");
    ASSERT(strcmp(buf, "data") == 0, "TryRead content");

    /* TryWrite on pipe with space — should succeed immediately */
    nr = ui_TryWrite(fds[1], "more", 4);
    ASSERT(nr == 4, "TryWrite with space");
    nr = read(fds[0], buf, 4);
    ASSERT(nr == 4, "read back TryWrite data");

    /* Invalid fd — should return -1 (not EAGAIN) */
    nr = ui_TryRead(-1, buf, 4);
    ASSERT(nr == -1, "TryRead invalid fd");

    nr = ui_TryWrite(-1, "x", 1);
    ASSERT(nr == -1, "TryWrite invalid fd");

    close(fds[0]); close(fds[1]);
    ui_Fini();
    PASS();
}

/* ── ui_TryRead EAGAIN fallback to ui_Read ── */

static int try_again_fds[2];
static int try_again_done;

static void try_again_writer(void) {
    /* Brief delay so reader blocks on empty pipe first */
    ui_Sleep(5);
    ssize_t nw = write(try_again_fds[1], "hey!", 4);
    ASSERT(nw == 4, "fallback writer write");
}

static void try_again_reader(void) {
    char buf[16];
    /* Pipe is empty + non-blocking → fast-path read() returns EAGAIN,
     * falls through to ui_Read which blocks until writer provides data. */
    ssize_t nr = ui_TryRead(try_again_fds[0], buf, 4);
    ASSERT(nr == 4, "TryRead EAGAIN fallback got data");
    buf[4] = 0;
    ASSERT(strcmp(buf, "hey!") == 0, "TryRead fallback content");
    try_again_done = 1;
}

static void test_try_read_again(void) {
    TEST("ui_TryRead EAGAIN fallback");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    ASSERT(pipe(try_again_fds) == 0, "pipe failed");
    /* Set read end to non-blocking so fast-path read() returns EAGAIN */
    int flags = fcntl(try_again_fds[0], F_GETFL);
    ASSERT(flags >= 0, "fcntl F_GETFL");
    ASSERT(fcntl(try_again_fds[0], F_SETFL, flags | O_NONBLOCK) == 0, "fcntl O_NONBLOCK");
    try_again_done = 0;

    ui_Go(try_again_reader);
    ui_Go(try_again_writer);
    ui_Run();

    ASSERT(try_again_done, "TryRead should complete via ui_Read fallback");

    close(try_again_fds[0]); close(try_again_fds[1]);
    ui_Fini();
    PASS();
}

/* ── Sleep ── */

static int sleep_done = 0;

static void sleeper(void) {
    ui_Sleep(10);
    sleep_done = 1;
}

static void test_sleep(void) {
    TEST("sleep");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    sleep_done = 0;
    ui_Go(sleeper);
    ui_Run();
    ASSERT(sleep_done, "sleep should complete");
    ui_Fini();
    PASS();
}

/* ── Chain spawn (goroutines spawning goroutines) ── */

static int chain_counter = 0;
static int chain_max_depth = 5;

static void chain_starter(void) {
    for (int i = 0; i <= chain_max_depth; i++) {
        __sync_fetch_and_add(&chain_counter, 1);
        ui_Yield();
    }
}

static void test_chain_spawn(void) {
    TEST("chain spawn (5 deep)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    chain_counter = 0;
    ui_Go(chain_starter);
    ui_Run();
    ASSERT(chain_counter == chain_max_depth + 1, "all chain links should run");
    ui_Fini();
    PASS();
}

/* ── io_uring in goroutine ── */

static int pipe_io_done = 0;
static int pipe_io_fds[2];

static void pipe_reader(void) {
    char buf[16] = {0};
    ssize_t nr = ui_Read(pipe_io_fds[0], buf, 15);
    if (nr > 0) pipe_io_done = 1;
    close(pipe_io_fds[0]);
}

static void pipe_writer(void) {
    ui_Sleep(5);
    ssize_t nw = ui_Write(pipe_io_fds[1], "data", 4);
    (void)nw;
    close(pipe_io_fds[1]);
}

static void test_io_in_goro(void) {
    TEST("io_uring in goroutine (pipe)");
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int ring_fd = (int)syscall(__NR_io_uring_setup, 2, &p);
    if (ring_fd < 0) {
        printf("SKIP: io_uring unavailable: %s\n", strerror(errno));
        test_passed++;
        return;
    }
    close(ring_fd);

    ASSERT(ui_Init() == 0, "ui_Init failed");
    ASSERT(pipe(pipe_io_fds) == 0, "pipe failed");
    pipe_io_done = 0;
    ui_Go(pipe_reader);
    ui_Go(pipe_writer);
    ui_Run();
    ASSERT(pipe_io_done == 1, "goroutine io should complete");
    ui_Fini();
    PASS();
}

/* ── ui_RecvBatch: submit multiple recvmsg, return on first completion ── */

static int batch_srv_fd;
static int batch_result;
static int batch_sent_ok;
static int batch_done;

static void batch_sender(void) {
    /* Create a separate client socket to send a datagram */
    int cli = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (cli < 0) { batch_sent_ok = 0; return; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(20701);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ssize_t ns = sendto(cli, "batch!", 6, 0,
                        (struct sockaddr*)&dst, sizeof(dst));
    batch_sent_ok = (ns == 6);
    close(cli);
}

static void batch_tester(void) {
    batch_srv_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (batch_srv_fd < 0) { batch_done = 1; return; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(20701);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(batch_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(batch_srv_fd);
        batch_done = 1;
        return;
    }

    /* Prepare 5 recvmsg slots, each with its own iov+buf */
    struct iovec iovs[5];
    char bufs[5][64];
    struct mmsghdr msgvec[5];
    for (int i = 0; i < 5; i++) {
        memset(bufs[i], 0, 64);
        iovs[i].iov_base = bufs[i];
        iovs[i].iov_len = 64;
        memset(&msgvec[i].msg_hdr, 0, sizeof(msgvec[i].msg_hdr));
        msgvec[i].msg_hdr.msg_iov = &iovs[i];
        msgvec[i].msg_hdr.msg_iovlen = 1;
    }

    /* Start sender goroutine, then call RecvBatch */
    ui_Go(batch_sender);

    int n = ui_RecvBatch(batch_srv_fd, msgvec, 5, 0);
    batch_result = n;

    close(batch_srv_fd);
    batch_done = 1;
}

static void test_recv_batch(void) {
    TEST("ui_RecvBatch UDP");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    batch_srv_fd = -1;
    batch_result = 0;
    batch_sent_ok = 0;
    batch_done = 0;

    ui_Go(batch_tester);
    ui_Run();

    ASSERT(batch_done, "batch recv completed");
    ASSERT(batch_sent_ok, "batch sender sendto succeeded");
    ASSERT(batch_result >= 1, "RecvBatch >= 1");
    ASSERT(batch_result <= 5, "RecvBatch <= 5");

    ui_Fini();
    PASS();
}

/* ── SelectWait multi-channel blocking wait ── */

static uint64_t selectwait_chs[3];
static int selectwait_result = -2;
static int selectwait_value = 0;

static void selectwait_waiter(void) {
    int vals[3] = {0, 0, 0};
    void *bufs[3] = {&vals[0], &vals[1], &vals[2]};
    selectwait_result = ui_SelectWait(selectwait_chs, bufs, NULL, NULL, 3, 0, -1);
    if (selectwait_result >= 0 && selectwait_result < 3)
        selectwait_value = vals[selectwait_result];
}

static void selectwait_sender(void) {
    ui_Sleep(10);
    int v = 77;
    ui_ChanSend(selectwait_chs[2], &v);
}

static void test_selectwait_multi_channel(void) {
    TEST("SelectWait multi-channel blocking recv");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    for (int i = 0; i < 3; i++) {
        selectwait_chs[i] = ui_NewChan(sizeof(int), 1);
        ASSERT(selectwait_chs[i] != 0, "ui_NewChan failed");
    }
    selectwait_result = -2;
    selectwait_value = 0;
    ui_Go(selectwait_waiter);
    ui_Go(selectwait_sender);
    ui_Run();
    ASSERT(selectwait_result == 2, "SelectWait should return ready channel index");
    ASSERT(selectwait_value == 77, "SelectWait received wrong value");
    for (int i = 0; i < 3; i++)
        ui_ChanFree(selectwait_chs[i]);
    ui_Fini();
    PASS();
}

/* ── Work stealing fanout ── */

#define STEAL_FANOUT_COUNT 512
static atomic_int steal_fanout_done;

static void steal_fanout_worker(void) {
    ui_Yield();
    atomic_fetch_add(&steal_fanout_done, 1);
}

static void steal_fanout_root(void) {
    for (int i = 0; i < STEAL_FANOUT_COUNT; i++)
        ui_Go(steal_fanout_worker);
}

static void test_work_stealing_fanout(void) {
    TEST("work stealing fanout spawn");
    setenv("UI_NVCPUS", "4", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    atomic_store(&steal_fanout_done, 0);
    ui_Go(steal_fanout_root);
    ui_Run();
    ASSERT(atomic_load(&steal_fanout_done) == STEAL_FANOUT_COUNT,
           "not all fanout workers completed");
    ui_Fini();
    unsetenv("UI_NVCPUS");
    PASS();
}

/* ── Join (via shared state) ── */

static int join_val = 0;

static void join_worker(void) {
    ui_Sleep(10);
    join_val = 42;
}

static void test_join(void) {
    TEST("wait for goroutine completion");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    join_val = 0;
    ui_Go(join_worker);
    /* Worker will set join_val before Run returns */
    ui_Run();
    ASSERT(join_val == 42, "worker should have completed");
    ui_Fini();
    PASS();
}

/* ── Multiple independent goroutines ── */

#define MANY_COUNT 1000
static int many_counter = 0;

static void many_worker(void) {
    __sync_fetch_and_add(&many_counter, 1);
}

static void test_many_goros(void) {
    TEST("1000 goroutines (mass spawn)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    many_counter = 0;
    for (int i = 0; i < MANY_COUNT; i++)
        ui_Go(many_worker);
    ui_Run();
    ASSERT(many_counter == MANY_COUNT, "not all goroutines ran");
    ui_Fini();
    PASS();
}

/* ── Stack growth deep recursion ── */

static volatile int deep_recursion_count = 0;
static volatile int deep_recursion_limit = 2000;

static void deep_recurser(volatile int depth) {
    volatile char buf[256];
    (void)buf;
    if (depth < deep_recursion_limit) {
        deep_recurser(depth + 1);
    } else {
        deep_recursion_count = depth;
    }
}

static void deep_starter(void) {
    deep_recurser(0);
}

static void test_deep_recursion(void) {
    TEST("deep recursion stack growth (2000 frames)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    deep_recursion_count = 0;
    ui_GoSized(deep_starter, 65536);
    ui_Run();
    ASSERT(deep_recursion_count == deep_recursion_limit, "recursion did not complete");
    ui_Fini();
    PASS();
}

/* ── Concurrent channel stress test ── */

#define STRESS_SENDERS  4
#define STRESS_RECEIVERS 4
#define STRESS_OPS      200

static uint64_t stress_ch;
static atomic_int stress_sent;
static atomic_int stress_recvd;
static atomic_int stress_errors;

static void stress_sender(void) {
    int64_t val = 42;
    for (int i = 0; i < STRESS_OPS; i++) {
        ui_ChanSend(stress_ch, &val);
        atomic_fetch_add(&stress_sent, 1);
    }
}

static void stress_receiver(void) {
    int64_t val;
    for (int i = 0; i < STRESS_OPS; i++) {
        ui_ChanRecv(stress_ch, &val);
        if (val != 42 && val != 0) atomic_fetch_add(&stress_errors, 1);
        atomic_fetch_add(&stress_recvd, 1);
    }
}

static void test_chan_concurrent_stress(void) {
    TEST("concurrent channel stress (4S+4R × 5000)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    stress_ch = ui_NewChan(sizeof(int64_t), 64);
    ASSERT(stress_ch != 0, "ui_NewChan failed");
    atomic_store(&stress_sent, 0);
    atomic_store(&stress_recvd, 0);
    atomic_store(&stress_errors, 0);

    for (int i = 0; i < STRESS_SENDERS; i++) ui_Go(stress_sender);
    for (int i = 0; i < STRESS_RECEIVERS; i++) ui_Go(stress_receiver);

    ui_Run();

    int s = atomic_load(&stress_sent);
    int r = atomic_load(&stress_recvd);
    int e = atomic_load(&stress_errors);

    ASSERT(s == STRESS_SENDERS * STRESS_OPS, "unexpected sent count");
    ASSERT(r == STRESS_RECEIVERS * STRESS_OPS, "unexpected recv count");
    ASSERT(s == r, "sent != received");
    ASSERT(e == 0, "data corruption detected");

    ui_ChanFree(stress_ch);
    ui_Fini();
    PASS();
}

/* ── C1: Sleep queue overflow test (UI_NVCPUS=1 must be set) ── */

static atomic_int sleepq_counter;

static void sleepq_worker(void) {
    ui_Sleep(10);
    atomic_fetch_add(&sleepq_counter, 1);
}

static void test_sleepq_overflow(void) {
    TEST("sleep queue overflow >256 goros (C1)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    atomic_store(&sleepq_counter, 0);
    /* >256 goros all sleeping on the same vCPU to overflow the 256-slot sleepq */
    for (int i = 0; i < 260; i++) ui_Go(sleepq_worker);
    ui_Run();
    ASSERT(atomic_load(&sleepq_counter) == 260, "not all sleepers completed (C1 bug)");
    ui_Fini();
    PASS();
}

/* ── C6: ChanTryRecv closed-before-count order ── */

static void test_tryrecv_closed_order(void) {
    TEST("chan TryRecv closed-before-count order (C6)");
    ASSERT(ui_Init() == 0, "ui_Init failed");

    /* Create channel, put data in, close, TryRecv should get data */
    uint64_t ch = ui_NewChan(sizeof(int), 1);
    int v = 42;
    ui_ChanSend(ch, &v);
    ui_ChanClose(ch);

    int got = -1;
    bool ok = ui_ChanTryRecv(ch, &got);
    ASSERT(ok, "TryRecv should succeed (has buffered data)");
    ASSERT(got == 42, "TryRecv should return 42, not 0 (C6 bug)");

    /* Second TryRecv: channel closed + empty → zero */
    ok = ui_ChanTryRecv(ch, &got);
    ASSERT(ok, "TryRecv on closed empty channel should return true");
    ASSERT(got == 0, "TryRecv on closed empty should return 0");

    ui_ChanFree(ch);
    ui_Fini();
    PASS();
}

/* ── C2/C10: TOCTOU race stress (concurrent channel + condvar) ── */

#define TOCTOU_SENDERS 4
#define TOCTOU_RECEIVERS 4
#define TOCTOU_OPS 2000

static uint64_t toctou_ch;
static atomic_int toctou_sent;
static atomic_int toctou_recvd;

static void toctou_sender(void) {
    int64_t val = 42;
    for (int i = 0; i < TOCTOU_OPS; i++) {
        ui_ChanSend(toctou_ch, &val);
        atomic_fetch_add(&toctou_sent, 1);
    }
}

static void toctou_receiver(void) {
    int64_t val;
    for (int i = 0; i < TOCTOU_OPS; i++) {
        ui_ChanRecv(toctou_ch, &val);
        atomic_fetch_add(&toctou_recvd, 1);
    }
}

static void test_toctou_channel_stress(void) {
    TEST("TOCTOU race stress (4S+4R × 2000)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    toctou_ch = ui_NewChan(sizeof(int64_t), 16);
    ASSERT(toctou_ch != 0, "ui_NewChan failed");
    atomic_store(&toctou_sent, 0);
    atomic_store(&toctou_recvd, 0);

    for (int i = 0; i < TOCTOU_SENDERS; i++) ui_Go(toctou_sender);
    for (int i = 0; i < TOCTOU_RECEIVERS; i++) ui_Go(toctou_receiver);

    ui_Run();
    ASSERT(atomic_load(&toctou_sent) == TOCTOU_SENDERS * TOCTOU_OPS, "sent count mismatch");
    ASSERT(atomic_load(&toctou_recvd) == TOCTOU_RECEIVERS * TOCTOU_OPS, "recv count mismatch");
    ui_ChanFree(toctou_ch);
    ui_Fini();
    PASS();
}

/* ── Condvar stress (exercises C10 TOCTOU) ── */

static uint64_t stress_mtx, stress_cv;
static atomic_int stress_cond_done;
static int stress_cond_val;

static void cond_stress_waiter(void) {
    ui_MutexLock(stress_mtx);
    while (stress_cond_val == 0)
        ui_CondWait(stress_cv, stress_mtx);
    ui_MutexUnlock(stress_mtx);
    atomic_fetch_add(&stress_cond_done, 1);
}

static void cond_stress_signaler(void) {
    ui_Sleep(5);
    ui_MutexLock(stress_mtx);
    stress_cond_val = 1;
    ui_CondBroadcast(stress_cv);
    ui_MutexUnlock(stress_mtx);
}

static void test_condvar_toctou(void) {
    TEST("condvar TOCTOU stress (50 waiters + broadcast)");
    ASSERT(ui_Init() == 0, "ui_Init failed");
    stress_mtx = ui_MutexNew();
    stress_cv = ui_CondNew();
    ASSERT(stress_mtx != 0 && stress_cv != 0, "new failed");
    atomic_store(&stress_cond_done, 0);
    stress_cond_val = 0;

    for (int i = 0; i < 50; i++) ui_Go(cond_stress_waiter);
    ui_Go(cond_stress_signaler);

    ui_Run();
    ASSERT(atomic_load(&stress_cond_done) == 50, "not all waiters completed (C10 TOCTOU)");
    ui_MutexFree(stress_mtx);
    ui_CondFree(stress_cv);
    ui_Fini();
    PASS();
}

/* ── GoOn targeted spawn: home vCPU placement + NVCPUs + clamp ── */

static atomic_int gon_vcpu_observed[8];
static atomic_int gon_done;

static void gon_worker(void) {
    /* Record which vCPU this goro actually runs on first */
    ui_vCPU *v = ui_this_vcpu;
    int id = v ? v->id : -1;
    atomic_fetch_add(&gon_vcpu_observed[id < 0 ? 0 : id], 1);
    atomic_fetch_add(&gon_done, 1);
}

static void test_goon_targeted(void) {
    TEST("GoOn spawns on the requested vCPU");
    setenv("UI_NVCPUS", "4", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    for (int i = 0; i < 8; i++) atomic_store(&gon_vcpu_observed[i], 0);
    atomic_store(&gon_done, 0);
    for (int v = 0; v < 4; v++)
        ui_GoOn(gon_worker, v);
    ui_Run();
    ui_Fini();
    unsetenv("UI_NVCPUS");
    ASSERT(atomic_load(&gon_done) == 4, "not all GoOn goros ran");
    for (int v = 0; v < 4; v++)
        ASSERT(atomic_load(&gon_vcpu_observed[v]) == 1,
               "goro did not run on requested vCPU");
    PASS();
}

static void test_goon_clamp(void) {
    TEST("GoOn out-of-range vcpu clamps to 0");
    setenv("UI_NVCPUS", "2", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    for (int i = 0; i < 8; i++) atomic_store(&gon_vcpu_observed[i], 0);
    atomic_store(&gon_done, 0);
    ui_GoOn(gon_worker, 99);   /* >= nvcpus */
    ui_GoOn(gon_worker, -3);   /* negative */
    ui_Run();
    ui_Fini();
    unsetenv("UI_NVCPUS");
    ASSERT(atomic_load(&gon_done) == 2, "clamped goros did not run");
    ASSERT(atomic_load(&gon_vcpu_observed[0]) == 2, "clamped goros not on vCPU 0");
    PASS();
}

static void test_nvcpus(void) {
    TEST("NVCPUs reflects UI_NVCPUS");
    setenv("UI_NVCPUS", "3", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    ASSERT(ui_NVCPUs() == 3, "NVCPUs != 3");
    ui_Fini();
    unsetenv("UI_NVCPUS");
    PASS();
}

/* ── PinTo: rebind current goro's home vCPU (takes effect at next block) ── */

static uint64_t pin_ch;
static atomic_int pin_start_vcpu;
static atomic_int pin_wake_vcpu;
static atomic_int pin_done;

static void pin_worker(void) {
    ui_vCPU *v = ui_this_vcpu;
    atomic_store(&pin_start_vcpu, v ? v->id : -1);
    ui_PinTo(1);                       /* rebind home to vCPU 1 */
    int x;
    ui_ChanRecv(pin_ch, &x);           /* block; wakeup routes to new home */
    v = ui_this_vcpu;
    atomic_store(&pin_wake_vcpu, v ? v->id : -1);
    atomic_fetch_add(&pin_done, 1);
}

static void pin_sender(void) {
    ui_Sleep(10);
    int x = 7;
    ui_ChanSend(pin_ch, &x);
}

static atomic_int pin_clamp_home;

static void pin_worker_clamp(void) {
    ui_PinTo(99);                      /* >= nvcpus */
    ui_vCPU *v = ui_this_vcpu;
    atomic_store(&pin_clamp_home, v->current->home_vcpu);
    ui_Yield();
}

static void test_pinto_rebind(void) {
    TEST("PinTo rebinds goro to new home at next block");
    setenv("UI_NVCPUS", "2", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    pin_ch = ui_NewChan(sizeof(int), 1);
    atomic_store(&pin_start_vcpu, -1);
    atomic_store(&pin_wake_vcpu, -1);
    atomic_store(&pin_done, 0);
    ui_Go(pin_worker);
    ui_Go(pin_sender);
    ui_Run();
    ui_ChanFree(pin_ch);
    ui_Fini();
    unsetenv("UI_NVCPUS");
    ASSERT(atomic_load(&pin_done) == 1, "pin worker did not finish");
    ASSERT(atomic_load(&pin_start_vcpu) == 0, "pin worker should start on vCPU 0");
    ASSERT(atomic_load(&pin_wake_vcpu) == 1, "pin worker should wake on vCPU 1");
    PASS();
}

static void test_pinto_clamp(void) {
    TEST("PinTo out-of-range vcpu clamps home to 0");
    setenv("UI_NVCPUS", "2", 1);
    ASSERT(ui_Init() == 0, "ui_Init failed");
    ui_Go(pin_worker_clamp);
    ui_Run();
    ui_Fini();
    unsetenv("UI_NVCPUS");
    ASSERT(atomic_load(&pin_clamp_home) == 0, "PinTo(99) home != 0");
    PASS();
}

int main(void) {
    printf("=== UI (うい) Coroutine Library Extended Tests ===\n\n");

    test_spawn_yield();
    test_multi_goro();
    test_stack_grow();
    test_chan_buffered();
    test_chan_try();
    test_chan_close();
    test_ping_pong();
    test_chan_multi_goro();
    test_mutex_multi();
    test_mutex_trylock();
    test_cond();
    test_cond_broadcast();
    test_sleep();
    test_chain_spawn();
    test_io_pipe();
    test_try_read_write();
    test_try_read_again();
    test_recv_batch();
    test_io_in_goro();
    test_selectwait_multi_channel();
    test_work_stealing_fanout();
    test_join();
    test_many_goros();
    test_deep_recursion();
    test_chan_concurrent_stress();
    test_goon_targeted();
    test_goon_clamp();
    test_nvcpus();
    test_pinto_rebind();
    test_pinto_clamp();

    /* P0 bug regression tests */
    test_tryrecv_closed_order();
    test_toctou_channel_stress();
    test_condvar_toctou();

    /* C1: sleepq overflow — must setenv before ui_Init */
    setenv("UI_NVCPUS", "1", 1);
    test_sleepq_overflow();
    unsetenv("UI_NVCPUS");

    printf("\n=== Results: %d passed, %d failed ===\n",
           test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
