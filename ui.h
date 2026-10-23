#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

typedef void (*ui_Func0)(void);
typedef void (*ui_Func1)(uintptr_t);

int      ui_Init(void);
void     ui_Run(void);
void     ui_Fini(void);

/* ── Stack memory policy ──
 *
 * A goroutine's stack is 8MB of address space, of which 128KB is
 * committed; how much of the committed part stays resident when the
 * goroutine goes back to the pool is a policy choice:
 *
 *   UI_STACK_TRIM_NONE     (default) keep the pages.  Fastest recycle;
 *                           the resident set follows the pool.
 *   UI_STACK_TRIM_FREE     MADV_FREE: the pages are marked free and
 *                           reclaimed lazily, when the kernel is short
 *                           of memory.
 *   UI_STACK_TRIM_DONTNEED MADV_DONTNEED: the pages are unmapped at
 *                           once, so the resident set tracks the live
 *                           set instead of the pool.
 *
 * The cost is dominated by the madvise call itself, not by the TLB
 * shootdown: measured on 4 vCPU at 400k steady-state spawns (median of
 * 3 runs),
 *
 *   NONE         298 ns/op   23.8 MB
 *   MADV_FREE   1994 ns/op   18.4 MB
 *   DONTNEED    3022 ns/op    2.6 MB
 *   NONE + 4MB   866 ns/op    5.7 MB
 *
 * MADV_FREE does hand back ~5MB of resident set, but it costs 6.5x the
 * latency to do it.  The memory budget is the better tool for that job:
 * NONE + a 4MB limit is both 2.3x faster AND 3x leaner than MADV_FREE,
 * because what actually returns the pages is dropping the pooled
 * stacks, not the advice.  MADV_FREE is the right choice only in a
 * deployment with no swap where the pool must stay large; everywhere
 * else, prefer the budget.
 *
 * ui_SetStackMemoryLimit() is the knob for a deployment that needs a
 * bound: alongside the strict policy it makes the pool drop stacks once
 * the budget is exhausted.  A limit of 0 leaves the policy alone.  The
 * budget covers the POOL; the stacks of *live* goroutines are not part
 * of it, and the accounting is an estimate (a lenient one), so treat
 * the budget as a soft cap.
 */
typedef enum {
    UI_STACK_TRIM_NONE     = 0,
    UI_STACK_TRIM_FREE     = 1,
    UI_STACK_TRIM_DONTNEED = 2,
} ui_StackTrimPolicy;

/* The iroha binding names these ui.StackTrimNone / ui.StackTrimFree /
 * ui.StackTrimDontNeed; see ui.iroha and the README. */

int         ui_SetStackTrimPolicy(ui_StackTrimPolicy policy);
int         ui_GetStackTrimPolicy(void);
int         ui_SetStackMemoryLimit(size_t bytes);
size_t      ui_StackMemoryLimit(void);

/* ── Runtime tuning ──
 *
 * Programmatic counterparts of the environment variables of the same
 * name (UI_NVCPUS, UI_YIELD_IO_MASK, UI_IDLE_WAIT_MS).  A value set
 * through the API wins over the environment, and over an earlier API
 * call; without either, the default applies.
 *
 * ui_SetNVCPUs() must be called before ui_Init() - that is when the
 * vCPU threads are created - and returns -1 with errno=EBUSY if the
 * runtime is already running, rather than silently doing nothing.
 *
 * The other two may be called at any time and take effect from the
 * next scheduling decision.  There is no synchronisation between the
 * call and the vCPU threads reading the value; a plain store is fine
 * here because a thread that sees either the old or the new value is
 * behaving as documented.
 *
 *   YieldIOMask - give up the vCPU after every Nth I/O completion, so
 *                 that a goroutine draining completions in a loop does
 *                 not starve its own run queue.  0 disables it.
 *   IdleWaitMs  - how long a parked vCPU waits before re-checking for
 *                 work it was never told about.  Every real wake source
 *                 (cross-vCPU wakeup, I/O completion, sleepq deadline)
 *                 signals directly, so this only covers a lost wakeup -
 *                 but it is also what decides the idle CPU cost, since
 *                 a vCPU that finds nothing to do waits this long.
 */
int         ui_SetNVCPUs(int n);

int         ui_SetYieldIOMask(uint32_t n);
uint32_t    ui_GetYieldIOMask(void);
int         ui_SetIdleWaitMs(uint32_t ms);
uint32_t    ui_GetIdleWaitMs(void);

uint64_t ui_Go(ui_Func0 f);
uint64_t ui_GoSized(ui_Func0 f, int stack_size);
uint64_t ui_Go1(void *fn, uintptr_t arg);
uint64_t ui_Go1Sized(void *fn, uintptr_t arg, int stack_size);

/* Spawn variants that report failure instead of aborting the process.
 * ui_Go() and friends abort when a goroutine cannot be created (out of
 * goroutine slots or address space) - a silently dropped task is a
 * correctness bug in the caller's workload.  Use these when the caller
 * can shed load: return 0 on success (the goroutine id is stored in
 * *id_out when id_out is non-NULL), -1 on failure with errno set
 * (ENOMEM for an exhausted runtime). */
int ui_TryGo(ui_Func0 f, uint64_t *id_out);
int ui_TryGoSized(ui_Func0 f, int stack_size, uint64_t *id_out);
int ui_TryGo1(void *fn, uintptr_t arg, uint64_t *id_out);

/* Targeted spawn: place the goro on an explicit home vCPU (soft binding —
 * the goro resumes there after blocking ops; stealing may still migrate
 * it).  Out-of-range vcpu clamps to 0. */
uint64_t ui_GoOn(ui_Func0 f, int vcpu);
uint64_t ui_GoOnSized(ui_Func0 f, int stack_size, int vcpu);
uint64_t ui_Go1On(void *fn, uintptr_t arg, int vcpu);
uint64_t ui_Go1OnSized(void *fn, uintptr_t arg, int stack_size, int vcpu);

int      ui_NVCPUs(void);

void     ui_Yield(void);
void     ui_Sleep(unsigned int ms);
void     ui_SleepUs(unsigned int us);

/* Rebind the current goro to an explicit home vCPU (soft binding, same
 * semantics as GoOn — takes effect from the next blocking point). */
void     ui_PinTo(int vcpu);

uint64_t ui_NewChan(size_t elem_size, unsigned int buf_cap);
void     ui_ChanSend(uint64_t c, const void *val);
void     ui_ChanRecv(uint64_t c, void *val);
bool     ui_ChanTrySend(uint64_t c, const void *val);
bool     ui_ChanTryRecv(uint64_t c, void *val);
void     ui_ChanClose(uint64_t c);
void     ui_ChanFree(uint64_t c);

uint64_t ui_NewTimer(unsigned int ms);
uint64_t ui_NewTimerUs(unsigned int us);
void     ui_TimerStop(uint64_t ch);
uint64_t ui_TimerReset(uint64_t old_ch, unsigned int new_us);

int      ui_SelectWait(const uint64_t *recv_chs, void **recv_bufs,
                       const uint64_t *send_chs, const void **send_vals,
                       int nrecv, int nsend, int timeout_ms);

uint64_t ui_MutexNew(void);
void     ui_MutexLock(uint64_t m);
bool     ui_MutexTryLock(uint64_t m);
void     ui_MutexUnlock(uint64_t m);
void     ui_MutexFree(uint64_t m);

uint64_t ui_CondNew(void);
void     ui_CondWait(uint64_t c, uint64_t m);
void     ui_CondSignal(uint64_t c);
void     ui_CondBroadcast(uint64_t c);
void     ui_CondFree(uint64_t c);

/* ── RwLock ── */
uint64_t ui_RwLockNew(void);
void     ui_RwLockRLock(uint64_t rw);
void     ui_RwLockRUnlock(uint64_t rw);
void     ui_RwLockWLock(uint64_t rw);
void     ui_RwLockWUnlock(uint64_t rw);
void     ui_RwLockFree(uint64_t rw);

ssize_t  ui_Read(int fd, void *buf, size_t count);
ssize_t  ui_Write(int fd, const void *buf, size_t count);
ssize_t  ui_TryRead(int fd, void *buf, size_t count);
ssize_t  ui_TryWrite(int fd, const void *buf, size_t count);
int      ui_Open(const char *pathname, int flags, ...);

ssize_t  ui_Recv(int fd, void *buf, size_t count, int flags);
ssize_t  ui_Send(int fd, const void *buf, size_t count, int flags);
ssize_t  ui_SendMsg(int fd, const struct msghdr *msg, int flags);
ssize_t  ui_RecvMsg(int fd, struct msghdr *msg, int flags);
int      ui_PollAdd(int fd, unsigned events);
int      ui_Connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int      ui_Accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int      ui_Close(int fd);
int      ui_Shutdown(int fd, int how);

/* ── Multishot recvmsg + buffer ring ── */

typedef void (*ui_RecvMultiCb)(void *ctx, struct sockaddr *from,
                               socklen_t from_len,
                               const void *data, size_t len);

struct ui_RecvMulti *ui_RecvMulti(int fd, ui_RecvMultiCb cb, void *ctx);
void                 ui_RecvMultiClose(struct ui_RecvMulti *rm);

/* ── Batch send/recv (UDP mmsg) ── */
int ui_SendMMsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags);
int ui_RecvMMsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags);
int ui_RecvBatch(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags);

#endif
