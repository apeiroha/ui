/* UI (うい) I/O benchmarks.
 *
 * Modeled on Go's testing.B (see tests/bench.h).  The metric names
 * the dashboard already tracks are kept - do not rename them without
 * also updating web/src/metrics.test.ts.
 *
 * Three things are measured here:
 *
 *   1. ui_Read when the data is already there (the fast path),
 *   2. ui_Read when the data is *not* there yet (a real park), next to
 *      the same round trip done with a plain POSIX read: that pair is
 *      what shows whether the latency belongs to the library or to the
 *      peer thread's wakeup,
 *   3. the cost of one io_uring_enter with 1, 8 and 64 submissions:
 *      the floor the library cannot go below, and the headroom that
 *      batching would buy.
 */
#include "ui.h"
#include "bench.h"

#include <fcntl.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int  io_pair[2];
static int  io_target;
static int  io_count;
static char io_buf[4096];

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

/* ── The park path ──
 *
 * The reader asks a plain thread for data and then blocks on it.  The
 * thread writes the answer, so the read has to park and be woken by a
 * completion.  The same round trip is then done with a plain POSIX
 * read() in a plain thread: the two numbers together say whether the
 * latency is the library's or the peer's wakeup.
 */

static int    park_ctrl[2];   /* reader -> peer: "send me something" */
static int    park_data[2];   /* peer -> reader: the payload */
static int    park_ops;

static void *
park_peer(void *arg)
{
    char buf[8];
    (void)arg;
    for (;;)
    {
        ssize_t n = read(park_ctrl[0], buf, 1);
        if (n <= 0) break;
        if (write(park_data[1], "x", 1) < 0) break;
    }
    return NULL;
}

static void
park_reader_goro(void)
{
    int done = 0;
    while (done < park_ops)
    {
        char c;
        if (write(park_ctrl[1], "g", 1) < 0) break;
        ssize_t n = ui_Read(park_data[0], &c, 1);
        if (n <= 0) break;
        done++;
    }
    io_count = done;
}

static void
bench_io_park(ui_bench_ctx *b, int n)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, park_ctrl) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, park_data) < 0)
    {
        perror("socketpair");
        ui_bench_set_ops(b, 0);
        return;
    }
    pthread_t peer;
    if (pthread_create(&peer, NULL, park_peer, NULL) != 0)
    {
        perror("pthread_create");
        ui_bench_set_ops(b, 0);
        return;
    }
    park_ops = n;
    io_count = 0;
    ui_Init();
    ui_bench_start(b);
    ui_Go(park_reader_goro);
    ui_Run();
    ui_bench_stop(b);
    ui_bench_set_ops(b, io_count);
    ui_Fini();
    close(park_ctrl[0]);
    close(park_ctrl[1]);
    close(park_data[0]);
    close(park_data[1]);
    pthread_join(peer, NULL);
}

/* The control: the same round trip without the library. */
static void *
posix_reader_thread(void *arg)
{
    int done = 0;
    int ops = (int)(intptr_t)arg;
    while (done < ops)
    {
        char c;
        if (write(park_ctrl[1], "g", 1) < 0) break;
        ssize_t n = read(park_data[0], &c, 1);
        if (n <= 0) break;
        done++;
    }
    io_count = done;
    return NULL;
}

static void
bench_posix_park(ui_bench_ctx *b, int n)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, park_ctrl) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, park_data) < 0)
    {
        perror("socketpair");
        ui_bench_set_ops(b, 0);
        return;
    }
    pthread_t peer, reader;
    if (pthread_create(&peer, NULL, park_peer, NULL) != 0 ||
        pthread_create(&reader, NULL, posix_reader_thread,
                       (void *)(intptr_t)n) != 0)
    {
        perror("pthread_create");
        ui_bench_set_ops(b, 0);
        return;
    }
    io_count = 0;
    ui_bench_start(b);
    pthread_join(reader, NULL);
    ui_bench_stop(b);
    ui_bench_set_ops(b, io_count);
    close(park_ctrl[0]);
    close(park_ctrl[1]);
    close(park_data[0]);
    close(park_data[1]);
    pthread_join(peer, NULL);
}

/* ── The io_uring floor ──
 *
 * What one io_uring_enter costs, with no library in the way: submit N
 * NOPs, enter, drain the completions.  With one SQE per enter the
 * syscall dominates; with 64 the same work costs a fraction of it.
 * That curve is the headroom a batching API would buy, and it is the
 * floor the library's own I/O path is measured against.
 */

struct uring_ctx {
    int                fd;
    struct io_uring_sqe *sqes;
    unsigned          *sq_tail;
    unsigned          *sq_head;
    unsigned          *sq_mask;
    unsigned          *cq_head;
    unsigned          *cq_tail;
    unsigned          *cq_mask;
    struct io_uring_cqe *cqes;
    void              *sq;
    void              *cq;
    size_t             sq_size;
    size_t             cq_size;
};

static int
uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int
uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                         NULL, 0);
}

static int
uring_open(struct uring_ctx *u)
{
    memset(u, 0, sizeof(*u));
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_COOP_TASKRUN | IORING_SETUP_NO_SQARRAY;
    u->fd = uring_setup(256, &p);
    if (u->fd < 0)
        return -1;
    u->sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    u->cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    u->sq = mmap(0, u->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                 u->fd, IORING_OFF_SQ_RING);
    u->cq = mmap(0, u->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                 u->fd, IORING_OFF_CQ_RING);
    u->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd,
                   IORING_OFF_SQES);
    if (u->sq == MAP_FAILED || u->cq == MAP_FAILED || u->sqes == MAP_FAILED)
        return -1;
    u->sq_tail = (unsigned *)((char *)u->sq + p.sq_off.tail);
    u->sq_head = (unsigned *)((char *)u->sq + p.sq_off.head);
    u->sq_mask = (unsigned *)((char *)u->sq + p.sq_off.ring_mask);
    u->cq_tail = (unsigned *)((char *)u->cq + p.cq_off.tail);
    u->cq_head = (unsigned *)((char *)u->cq + p.cq_off.head);
    u->cq_mask = (unsigned *)((char *)u->cq + p.cq_off.ring_mask);
    u->cqes = (struct io_uring_cqe *)((char *)u->cq + p.cq_off.cqes);
    return 0;
}

static void
uring_close(struct uring_ctx *u)
{
    if (u->sq && u->sq != MAP_FAILED) munmap(u->sq, u->sq_size);
    if (u->cq && u->cq != MAP_FAILED) munmap(u->cq, u->cq_size);
    if (u->sqes && u->sqes != MAP_FAILED)
        munmap(u->sqes, 256 * sizeof(struct io_uring_sqe));
    if (u->fd >= 0) close(u->fd);
}

static void
bench_uring_floor(ui_bench_ctx *b, int n)
{
    struct uring_ctx u;
    if (uring_open(&u) < 0)
    {
        perror("io_uring_setup");
        ui_bench_set_ops(b, 0);
        return;
    }
    int batch = *(const int *)b->arg;
    if (batch < 1) batch = 1;
    unsigned tail = 0;
    unsigned drained = 0;

    ui_bench_start(b);
    for (int i = 0; i < n; i += batch)
    {
        for (int k = 0; k < batch; k++)
        {
            struct io_uring_sqe *sqe = &u.sqes[(i + k) & *u.sq_mask];
            memset(sqe, 0, sizeof(*sqe));
            sqe->opcode = IORING_OP_NOP;
            sqe->fd = -1;
            sqe->user_data = (uint64_t)(i + k);
        }
        tail += (unsigned)batch;
        __atomic_store_n(u.sq_tail, tail, __ATOMIC_RELEASE);
        uring_enter(u.fd, (unsigned)batch, 0, IORING_ENTER_GETEVENTS);
        unsigned h = __atomic_load_n(u.cq_head, __ATOMIC_ACQUIRE);
        unsigned t = __atomic_load_n(u.cq_tail, __ATOMIC_ACQUIRE);
        while (h != t)
        {
            h++;
            drained++;
        }
        __atomic_store_n(u.cq_head, h, __ATOMIC_RELEASE);
    }
    ui_bench_stop(b);

    ui_bench_set_ops(b, (int)drained);
    ui_bench_report_extra(b, "sqe_per_enter", (double)batch, "count");
    uring_close(&u);
}

/* ── Main ── */

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("UI I/O benchmarks\n");
    printf("=================\n\n");

    ui_bench_add("bench_ui_io", "ui_Read (always-available data)",
                 bench_io_fastpath, NULL, 100000);
    ui_bench_add("bench_ui_io", "ui_Read (data not ready, park)",
                 bench_io_park, NULL, 2000);
    ui_bench_add("bench_ui_io", "POSIX read() (data not ready, park)",
                 bench_posix_park, NULL, 2000);

    static const int batch1 = 1, batch8 = 8, batch64 = 64;
    ui_bench_add("bench_ui_io", "io_uring enter floor (1 SQE)",
                 bench_uring_floor, (void *)&batch1, 20000);
    ui_bench_add("bench_ui_io", "io_uring enter floor (8 SQE)",
                 bench_uring_floor, (void *)&batch8, 20000);
    ui_bench_add("bench_ui_io", "io_uring enter floor (64 SQE)",
                 bench_uring_floor, (void *)&batch64, 20000);

    int rc = ui_bench_main(argc, argv);
    printf("\nDone.\n");
    return rc;
}
