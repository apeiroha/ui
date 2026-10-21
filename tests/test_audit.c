/* Regression tests for the C-runtime audit (see .opencode/report.md).
 *
 * Each risky case runs in a forked child with a wall-clock watchdog, so a
 * deadlock is reported as a test failure instead of hanging the suite.
 */
#include "ui_internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef UI_DEBUG_FOREIGN_DRAIN
extern int ui_debug_foreign_drain;
#endif
#ifdef UI_DEBUG_RECVBATCH
extern int ui_debug_recvbatch_live;
#endif

static int failures;
static int passes;

#define PASS(name) do { printf("  TEST: %s ... PASS\n", name); passes++; } while (0)
#define FAIL(name, why) do { \
    printf("  TEST: %s ... FAIL: %s\n", name, why); failures++; \
} while (0)

static int
wait_child(pid_t pid, int timeout_ms, int *code)
{
    for (int waited = 0; waited < timeout_ms; waited += 10)
    {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
        {
            *code = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
            return 0;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void
run_expect(void (*fn)(void), int want, int timeout_ms, const char *name)
{
    pid_t pid = fork();
    if (pid < 0) { FAIL(name, "fork failed"); return; }
    if (pid == 0)
    {
        fn();
        _exit(0);
    }
    int code = 0;
    if (wait_child(pid, timeout_ms, &code) < 0)
    {
        FAIL(name, "hang (watchdog)");
        return;
    }
    if (code != want)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "exit %d, want %d", code, want);
        FAIL(name, buf);
        return;
    }
    PASS(name);
}

/* ── R1: buffered channel FIFO ── */

static uint64_t r1_ch;
static int r1_a, r1_b, r1_ok;

static void r1_sender(void)
{
    int v = 20;
    ui_ChanSend(r1_ch, &v);
}

static void r1_logic(void)
{
    r1_ch = ui_NewChan(sizeof(int), 1);
    if (!r1_ch) return;
    int v = 10;
    ui_ChanSend(r1_ch, &v);
    ui_Go(r1_sender);
    ui_Yield();
    ui_ChanRecv(r1_ch, &r1_a);
    ui_ChanRecv(r1_ch, &r1_b);
    r1_ok = (r1_a == 10 && r1_b == 20);
}

static void r1_child(void)
{
    setenv("UI_NVCPUS", "1", 1);
    if (ui_Init() != 0) _exit(2);
    ui_Go(r1_logic);
    ui_Run();
    ui_Fini();
    _exit(r1_ok ? 0 : 1);
}

/* ── R2: select lost wake across vCPUs ── */

#define R2_NCH 1024
#define R2_VIEW_FIELDS \
    void *buf; size_t elem_size; unsigned cap, count, read_idx, write_idx; \
    int closed; pthread_spinlock_t lock; ui_WaitQ send_wait; ui_WaitQ recv_wait

typedef struct {
    void *buf;
    size_t elem_size;
    unsigned cap, count, read_idx, write_idx;
    int closed;
    pthread_spinlock_t lock;
    ui_WaitQ send_wait;
    ui_WaitQ recv_wait;
} ChanView;

static uint64_t r2_ch[R2_NCH];
static int r2_vals[R2_NCH];
static void *r2_bufs[R2_NCH];
static volatile int r2_ready;
static int r2_idx = -2;

static void r2_waiter(void)
{
    while (!__atomic_load_n(&r2_ready, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
    r2_idx = ui_SelectWait(r2_ch, r2_bufs, NULL, NULL, R2_NCH, 0, -1);
}

static void r2_sender(void)
{
    ChanView *c = (ChanView *)(uintptr_t)r2_ch[0];
    __atomic_store_n(&r2_ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&c->recv_wait.head, __ATOMIC_ACQUIRE) == NULL)
        __builtin_ia32_pause();
    int v = 77;
    ui_ChanSend(r2_ch[0], &v);
}

static void r2_child(void)
{
    setenv("UI_NVCPUS", "2", 1);
    if (ui_Init() != 0) _exit(2);
    for (int i = 0; i < R2_NCH; i++)
    {
        r2_ch[i] = ui_NewChan(sizeof(int), 4);
        if (!r2_ch[i]) _exit(2);
        r2_bufs[i] = &r2_vals[i];
    }
    ui_GoOn(r2_waiter, 0);
    ui_GoOn(r2_sender, 1);
    ui_Run();
    ui_Fini();
    _exit(r2_idx == 0 ? 0 : 1);
}

/* ── R3: PinTo + select timeout must not leak a sleepq entry ── */

static uint64_t r3_ch;
static int r3_ok;

static void r3_waiter(void)
{
    ui_PinTo(1);
    ui_Goro *self = ui_this_vcpu->current;
    int v = 0;
    void *buf = &v;
    int idx = ui_SelectWait(&r3_ch, &buf, NULL, NULL, 1, 0, 1000);
    int stale = (g_ui_sched.vcpus[0].sleepq_size > 0 &&
                 g_ui_sched.vcpus[0].sleepq[0] == self);
    r3_ok = (idx == 0 && !stale);
    _exit(r3_ok ? 0 : 1);
}

static void r3_sender(void)
{
    ui_Sleep(100);
    int v = 7;
    ui_ChanSend(r3_ch, &v);
}

static void r3_child(void)
{
    setenv("UI_NVCPUS", "2", 1);
    if (ui_Init() != 0) _exit(2);
    r3_ch = ui_NewChan(sizeof(int), 1);
    if (!r3_ch) _exit(2);
    ui_GoOn(r3_waiter, 0);
    ui_GoOn(r3_sender, 1);
    ui_Run();
    _exit(1);
}

/* ── R4: stack slot reuse with a larger commit ── */

static void r4_child(void)
{
    if (ui_Init() != 0) _exit(2);
    ui_Goro *a = calloc(1, sizeof(*a));
    if (!a) _exit(2);
    a->sleepq_idx = -1;
    if (ui_stack_init(a, 64 * 1024) != 0) _exit(2);
    volatile char *top = (volatile char *)ui_stack_bottom(a);
    top[-32 * 1024] = 1;
    ui_stack_destroy(a);

    ui_Goro *b = calloc(1, sizeof(*b));
    if (!b) _exit(2);
    b->sleepq_idx = -1;
    if (ui_stack_init(b, 1024 * 1024) != 0) _exit(2);
    volatile char *top_b = (volatile char *)ui_stack_bottom(b);
    top_b[-128 * 1024] = 1;
    _exit(0);
}

/* ── R5/R6: RecvBatch accounting and lifetime ── */

static void r5_logic(void);

static void r5_child(void)
{
    setenv("UI_NVCPUS", "1", 1);
    if (ui_Init() != 0) _exit(2);
    ui_Go(r5_logic);
    ui_Run();
    ui_Fini();
    _exit(0);
}

static void r5_logic(void)
{
    ui_vCPU *v = ui_this_vcpu;
    if (ui_vcpu_ensure_ring(v) < 0) { puts("  (skip: no io_uring)"); _exit(0); }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) _exit(2);
    char byte = 'x';
    if (send(sv[1], &byte, 1, 0) != 1) _exit(2);

    struct iovec iov[4];
    char bufs[4][64];
    struct mmsghdr msgvec[4];
    memset(msgvec, 0, sizeof(msgvec));
    for (int i = 0; i < 4; i++)
    {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = sizeof(bufs[i]);
        msgvec[i].msg_hdr.msg_iov = &iov[i];
        msgvec[i].msg_hdr.msg_iovlen = 1;
    }

    int n = ui_RecvBatch(sv[0], msgvec, 4, 0);
    ui_Sleep(150);
#ifdef UI_DEBUG_RECVBATCH
    int live = __atomic_load_n(&ui_debug_recvbatch_live, __ATOMIC_RELAXED);
#else
    int live = 0;
#endif
    _exit(n == 1 && v->uring_pending == 0 && live == 0 ? 0 : 1);
}

/* ── R7: RecvMultiClose from a non-owner vCPU ── */

static int r7_sv[2];
static uint64_t r7_go;
static struct ui_RecvMulti *r7_rm;

static void r7_cb(void *ctx, struct sockaddr *from, socklen_t from_len,
                  const void *data, size_t len)
{
    (void)ctx; (void)from; (void)from_len; (void)data; (void)len;
}

static void r7_creator(void)
{
    r7_rm = ui_RecvMulti(r7_sv[0], r7_cb, NULL);
    int v = 1;
    ui_ChanSend(r7_go, &v);
}

static void r7_closer(void)
{
    ui_PinTo(1);
    int v = 0;
    ui_ChanRecv(r7_go, &v);
    if (!r7_rm) _exit(0);   /* no multishot support: skip */
    ui_RecvMultiClose(r7_rm);
    ui_Sleep(150);
    int ok = (g_ui_sched.vcpus[0].active_multishot == NULL) &&
             (g_ui_sched.vcpus[1].retired_multishot == NULL) &&
             (g_ui_sched.vcpus[1].active_multishot == NULL);
    _exit(ok ? 0 : 1);
}

static void r7_child(void)
{
    setenv("UI_NVCPUS", "2", 1);
    if (ui_Init() != 0) _exit(2);
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, r7_sv) < 0) _exit(2);
    r7_go = ui_NewChan(sizeof(int), 1);
    ui_GoOn(r7_creator, 0);
    ui_GoOn(r7_closer, 1);
    ui_Run();
    _exit(1);
}

/* ── R8: multishot must keep receiving past the initial 256 buffers ── */

#define R8_NPKT 300
static int r8_sv[2];
static atomic_int r8_cb;
static struct ui_RecvMulti *r8_rm;

static void r8_callback(void *ctx, struct sockaddr *from, socklen_t from_len,
                        const void *data, size_t len)
{
    (void)ctx; (void)from; (void)from_len; (void)data; (void)len;
    atomic_fetch_add(&r8_cb, 1);
}

static void r8_receiver(void)
{
    r8_rm = ui_RecvMulti(r8_sv[0], r8_callback, NULL);
    if (!r8_rm) { puts("  (skip: no multishot)"); _exit(0); }
    ui_Sleep(700);
    int n = atomic_load(&r8_cb);
    _exit(n >= R8_NPKT ? 0 : 1);
}

static void r8_sender(void)
{
    ui_Sleep(100);
    char pkt[16] = "x";
    int sent = 0;
    while (sent < R8_NPKT)
    {
        ssize_t n = send(r8_sv[1], pkt, sizeof(pkt), 0);
        if (n == (ssize_t)sizeof(pkt)) sent++;
        else ui_SleepUs(100);
    }
}

static void r8_child(void)
{
    setenv("UI_NVCPUS", "2", 1);
    if (ui_Init() != 0) _exit(2);
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, r8_sv) < 0) _exit(2);
    ui_GoOn(r8_receiver, 0);
    ui_GoOn(r8_sender, 1);
    ui_Run();
    _exit(1);
}

/* ── R9: select send on a closed channel must be fatal, not hang ── */

static void r9_logic(void);

static void r9_child(void)
{
    setenv("UI_NVCPUS", "1", 1);
    if (ui_Init() != 0) _exit(2);
    ui_Go(r9_logic);
    ui_Run();
    _exit(0);   /* select returned: regression */
}

static void r9_logic(void)
{
    uint64_t ch = ui_NewChan(sizeof(int), 1);
    ui_ChanClose(ch);
    int v = 1;
    uint64_t sc[1] = { ch };
    const void *sv[1] = { &v };
    (void)ui_SelectWait(NULL, NULL, sc, sv, 0, 1, -1);
}

/* ── R10: PinTo + blocking I/O must not drain the old vCPU's ring ── */

static int r10_pr, r10_pw;
static int r10_ok;

static void r10_reader(void)
{
    if (ui_vcpu_ensure_ring(ui_this_vcpu) < 0) { _exit(0); }
    ui_PinTo(1);
    char c = 0;
    ssize_t n = ui_Read(r10_pr, &c, 1);
    int v = ui_this_vcpu->id;
#ifdef UI_DEBUG_FOREIGN_DRAIN
    int foreign = __atomic_load_n(&ui_debug_foreign_drain, __ATOMIC_RELAXED);
#else
    int foreign = 0;
#endif
    r10_ok = (n == 1 && v == 1 && foreign == 0);
    _exit(r10_ok ? 0 : 1);
}

static void r10_writer(void)
{
    ui_Sleep(100);
    char c = 'x';
    (void)write(r10_pw, &c, 1);
}

static void r10_child(void)
{
    setenv("UI_NVCPUS", "2", 1);
    if (ui_Init() != 0) _exit(2);
    int fds[2];
    if (pipe(fds) < 0) _exit(2);
    r10_pr = fds[0];
    r10_pw = fds[1];
    ui_GoOn(r10_reader, 0);
    ui_GoOn(r10_writer, 1);
    ui_Run();
    _exit(1);
}

/* ── R12: contended sync from a non-goro thread must not fake success ── */

static uint64_t r12_m;
static atomic_int r12_ready;

static void *r12_holder(void *arg)
{
    (void)arg;
    ui_MutexLock(r12_m);
    atomic_store(&r12_ready, 1);
    for (;;) usleep(1000);
    return NULL;
}

static void r12_child(void)
{
    if (ui_Init() != 0) _exit(2);
    r12_m = ui_MutexNew();
    if (!r12_m) _exit(2);
    pthread_t th;
    if (pthread_create(&th, NULL, r12_holder, NULL) != 0) _exit(2);
    while (!atomic_load(&r12_ready)) usleep(1000);
    ui_MutexLock(r12_m);   /* must abort: no goro to park */
    _exit(0);              /* returned: regression */
}

/* ── driver ── */

int
main(void)
{
    puts("=== UI audit regression tests ===");
    run_expect(r1_child, 0, 2000, "R1 buffered channel FIFO");
    run_expect(r2_child, 0, 3000, "R2 select lost wake across vCPUs");
    run_expect(r3_child, 0, 3000, "R3 PinTo + select timeout sleepq");
    run_expect(r4_child, 0, 3000, "R4 stack slot larger-commit reuse");
    run_expect(r5_child, 0, 3000, "R5/R6 RecvBatch pending+leak");
    run_expect(r7_child, 0, 3000, "R7 RecvMultiClose from foreign vCPU");
    run_expect(r8_child, 0, 3000, "R8 multishot buffer replenishment");
    run_expect(r9_child, 1, 3000, "R9 select send on closed channel is fatal");
    run_expect(r10_child, 0, 3000, "R10 PinTo + blocking I/O drain");
    run_expect(r12_child, 1, 3000, "R12 contended sync from non-goro is fatal");

    printf("\n=== Results: %d passed, %d failed ===\n", passes, failures);
    return failures ? 1 : 0;
}
