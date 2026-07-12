#include "ui_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <poll.h>
#include <linux/io_uring.h>

#ifndef IORING_FEAT_NO_SQARRAY
#define IORING_FEAT_NO_SQARRAY (1U << 10)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif

#define UI_URING_ENTRIES 1024
#define UI_IO_TIMEOUT_MS 5000

/* Buffer ring defaults */
#define UI_BUF_RING_COUNT    256
#define UI_BUF_RING_BUF_SIZE 2048

/* Extract fields from io_uring_recvmsg_out stored in buffer ring.
 * The kernel reserves msg_namelen bytes for the source address after the
 * header, regardless of the actual namelen written to recvmsg_out. */
static inline struct io_uring_recvmsg_out *
ui_recvmsg_out(void *buf)
{
    return (struct io_uring_recvmsg_out *)buf;
}
static inline struct sockaddr *
ui_recvmsg_name(struct io_uring_recvmsg_out *o, uint32_t reserved_namelen)
{
    (void)reserved_namelen;
    return (struct sockaddr *)((unsigned char *)o + sizeof(*o));
}
static inline void *
ui_recvmsg_payload(struct io_uring_recvmsg_out *o, uint32_t reserved_namelen,
                   uint32_t reserved_controllen)
{
    return (unsigned char *)o + sizeof(*o) + reserved_namelen + reserved_controllen;
}
static inline socklen_t
ui_recvmsg_namelen(struct io_uring_recvmsg_out *o)
{
    return o->namelen;
}
static inline size_t
ui_recvmsg_payloadlen(struct io_uring_recvmsg_out *o)
{
    return o->payloadlen;
}

static int
ui_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

int
ui_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
               unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

int
ui_vcpu_ensure_ring(ui_vCPU *v)
{
    if (v->ring_fd >= 0)
        return 0;

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    /* Each vCPU is the sole issuer of its ring. DEFER_TASKRUN defers
     * completion processing to explicit io_uring_enter(GETEVENTS) calls,
     * eliminating kernel IPIs and improving cache locality. */
    p.flags = IORING_SETUP_SINGLE_ISSUER |
              IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_COOP_TASKRUN |
              IORING_SETUP_NO_SQARRAY;

    v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    if (v->ring_fd < 0)
    {
        /* Fallback: kernel too old for the flags above */
        memset(&p, 0, sizeof(p));
        p.flags = 0;
        v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    }
    if (v->ring_fd < 0)
        return -1;

    v->no_sq_array = (p.flags & IORING_SETUP_NO_SQARRAY) != 0;

    size_t sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t sqes_size = p.sq_entries * sizeof(struct io_uring_sqe);

    void *sq_ptr = MAP_FAILED;
    void *cq_ptr = MAP_FAILED;

    sq_ptr = mmap(0, sq_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, v->ring_fd,
                  IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) goto err;

    cq_ptr = mmap(0, cq_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, v->ring_fd,
                  IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) goto err;

    v->sq_sqes = mmap(0, sqes_size,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, v->ring_fd,
                      IORING_OFF_SQES);
    if (v->sq_sqes == MAP_FAILED) goto err;

    v->sq_ring_ptr = sq_ptr;
    v->cq_ring_ptr = cq_ptr;
    v->sq_ring_size = sq_size;
    v->cq_ring_size = cq_size;
    v->sqes_size = sqes_size;

    v->sq_head = (unsigned *)(sq_ptr + p.sq_off.head);
    v->sq_tail = (unsigned *)(sq_ptr + p.sq_off.tail);
    v->sq_ring_mask = (unsigned *)(sq_ptr + p.sq_off.ring_mask);
    v->sq_ring_entries = (unsigned *)(sq_ptr + p.sq_off.ring_entries);
    v->sq_flags = (unsigned *)(sq_ptr + p.sq_off.flags);
    v->sq_array = (unsigned *)(sq_ptr + p.sq_off.array);

    v->cq_head = (unsigned *)(cq_ptr + p.cq_off.head);
    v->cq_tail = (unsigned *)(cq_ptr + p.cq_off.tail);
    v->cq_ring_mask = (unsigned *)(cq_ptr + p.cq_off.ring_mask);
    v->cq_ring_entries = (unsigned *)(cq_ptr + p.cq_off.ring_entries);
    v->cq_cqes = (struct io_uring_cqe *)(cq_ptr + p.cq_off.cqes);

    return 0;

err:
    if (v->sq_sqes && v->sq_sqes != MAP_FAILED)
        munmap(v->sq_sqes, sqes_size);
    if (cq_ptr && cq_ptr != MAP_FAILED)
        munmap(cq_ptr, cq_size);
    if (sq_ptr && sq_ptr != MAP_FAILED)
        munmap(sq_ptr, sq_size);
    if (v->ring_fd >= 0) close(v->ring_fd);
    v->ring_fd = -1;
    return -1;
}

/* ── Provided buffer ring for multishot recvmsg ── */

int
ui_vcpu_ensure_buf_ring(ui_vCPU *v)
{
    if (v->bgid >= 0)
        return 0;
    if (v->ring_fd < 0)
        return -1;

    v->bgid = v->id;
    v->buf_ring_count = UI_BUF_RING_COUNT;
    v->buf_ring_buf_size = UI_BUF_RING_BUF_SIZE;

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr    = 0;
    reg.ring_entries = (uint32_t)v->buf_ring_count;
    reg.bgid         = (uint16_t)v->bgid;
    reg.flags        = IOU_PBUF_RING_MMAP;

    int ret = (int)syscall(__NR_io_uring_register, v->ring_fd,
                           IORING_REGISTER_PBUF_RING, &reg, 1);
    if (ret < 0)
    {
        v->bgid = -1;
        return -1;
    }

    off_t mmap_offset = IORING_OFF_PBUF_RING |
                        ((uint64_t)(uint32_t)v->bgid << IORING_OFF_PBUF_SHIFT);
    v->buf_ring_mmap_sz = sizeof(struct io_uring_buf_ring) +
                          (size_t)v->buf_ring_count * sizeof(struct io_uring_buf);
    v->buf_ring = mmap(0, v->buf_ring_mmap_sz,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       v->ring_fd, mmap_offset);
    if (v->buf_ring == MAP_FAILED)
        goto err_unreg;

    size_t data_size = (size_t)v->buf_ring_count * (size_t)v->buf_ring_buf_size;
    v->buf_ring_bufs = mmap(0, data_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (v->buf_ring_bufs == MAP_FAILED)
        goto err_munmap;

    for (int i = 0; i < v->buf_ring_count; i++)
    {
        v->buf_ring->bufs[i].addr =
            (uint64_t)(uintptr_t)((unsigned char *)v->buf_ring_bufs +
                                  (size_t)i * (size_t)v->buf_ring_buf_size);
        v->buf_ring->bufs[i].len = (uint32_t)v->buf_ring_buf_size;
        v->buf_ring->bufs[i].bid = (uint16_t)i;
        v->buf_ring->bufs[i].resv = 0;
    }
    __sync_synchronize();
    v->buf_ring->tail = (uint16_t)v->buf_ring_count;
    return 0;

err_munmap:
    munmap(v->buf_ring, v->buf_ring_mmap_sz);
    v->buf_ring = NULL;
err_unreg:
    syscall(__NR_io_uring_register, v->ring_fd,
            IORING_UNREGISTER_PBUF_RING, &reg, 1);
    v->bgid = -1;
    return -1;
}

void
ui_vcpu_destroy_buf_ring(ui_vCPU *v)
{
    if (v->bgid < 0)
        return;

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_entries = (uint32_t)v->buf_ring_count;
    reg.bgid         = (uint16_t)v->bgid;
    syscall(__NR_io_uring_register, v->ring_fd,
            IORING_UNREGISTER_PBUF_RING, &reg, 1);

    if (v->buf_ring && v->buf_ring != MAP_FAILED)
        munmap(v->buf_ring, v->buf_ring_mmap_sz);
    if (v->buf_ring_bufs && v->buf_ring_bufs != MAP_FAILED)
        munmap(v->buf_ring_bufs,
               (size_t)v->buf_ring_count * (size_t)v->buf_ring_buf_size);
    v->buf_ring = NULL;
    v->buf_ring_bufs = NULL;
    v->bgid = -1;
    v->buf_ring_count = 0;
    v->buf_ring_buf_size = 0;
}

static struct io_uring_sqe *
ui_uring_get_sqe(ui_vCPU *v)
{
    unsigned head = *v->sq_head;
    unsigned next_tail = *v->sq_tail + 1;
    if (next_tail - head > *v->sq_ring_entries)
        return NULL;
    struct io_uring_sqe *sqe = &v->sq_sqes[*v->sq_tail & *v->sq_ring_mask];
    memset(sqe, 0, sizeof(*sqe));
    return sqe;
}

/* Submit a single SQE to the kernel */
static void
ui_uring_submit(ui_vCPU *v)
{
    unsigned tail = *v->sq_tail;
    unsigned mask = *v->sq_ring_mask;

    /* NO_SQARRAY: kernel reads SQEs directly from sq_sqes array */
    if (!v->no_sq_array)
        v->sq_array[tail & mask] = tail & mask;

    /* Write barrier: ensure SQE data + sq_array are visible before tail update */
    __sync_synchronize();

    /* Advance tail: kernel is now allowed to see and consume this SQE */
    *v->sq_tail = tail + 1;

    /* Track pending requests */
    v->uring_pending++;

    /* Submit via single syscall, flush pending completions */
    ui_uring_enter(v->ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
}

/* ── Idle wait: drain io_uring completions then block on eventfd ── */

/* Drain pending io_uring completions, then block via ppoll(eventfd)
 * until timeout or cross-vCPU wakeup.  The ppoll approach avoids
 * the complexity of POLL_ADD + TIMEOUT SQEs and is simpler while
 * still improving over the original dual-fd ppoll(ring_fd+eventfd). */
void
ui_uring_idle_wait(ui_vCPU *v, uint64_t wait_us)
{
    /* Flush deferred completions via GETEVENTS (required by DEFER_TASKRUN).
     * This must happen BEFORE blocking on eventfd so that I/O completions
     * that arrived during ui_schedule() are reaped. */
    if (v->ring_fd >= 0)
    {
        ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
    }

    struct timespec ts = {
        .tv_sec = (time_t)(wait_us / 1000000),
        .tv_nsec = (long)(wait_us % 1000000) * 1000,
    };
    struct pollfd pfd = { .fd = v->event_fd, .events = POLLIN };

    int ret = ppoll(&pfd, 1, &ts, NULL);
    if (ret > 0)
    {
        uint64_t val;
        while (read(v->event_fd, &val, sizeof(val)) == sizeof(val))
            ;
    }

    /* On wakeup, re-flush completions (I/O may have completed while
     * we were blocked; ppoll didn't check ring_fd). */
    if (v->ring_fd >= 0)
    {
        ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
        ui_uring_drain(v);
    }
}

/* Drain all available CQEs, waking goroutines */
void
ui_uring_drain(ui_vCPU *v)
{
    if (!v || v->ring_fd < 0)
        return;

    unsigned head = *v->cq_head;
    unsigned tail = *v->cq_tail;
    unsigned mask = *v->cq_ring_mask;

    while (head != tail)
    {
        struct io_uring_cqe *cqe = &v->cq_cqes[head & mask];

        if (cqe->user_data & 1)
        {
            /* ── Multishot recvmsg completion ── */
            struct ui_RecvMulti *rm = (struct ui_RecvMulti *)(uintptr_t)(cqe->user_data & ~1ULL);
            if (!rm->active)
                goto skip;

            if (cqe->flags & IORING_CQE_F_BUFFER && cqe->res > 0)
            {
                int bid = (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                void *buf = (unsigned char *)v->buf_ring_bufs +
                            (size_t)bid * (size_t)v->buf_ring_buf_size;
                struct io_uring_recvmsg_out *o = ui_recvmsg_out(buf);

                rm->cb(rm->ctx,
                       ui_recvmsg_name(o, rm->msg_namelen),
                       ui_recvmsg_namelen(o),
                       ui_recvmsg_payload(o, rm->msg_namelen,
                                           rm->msg_controllen),
                       ui_recvmsg_payloadlen(o));
            }
            else if (cqe->res < 0)
            {
                /* Error or cancellation */
                rm->active = 0;
            }
        }
        else if (cqe->user_data != 0)
        {
            /* ── Regular goro I/O completion ──
             * CAS on io_pending (1→0) ensures we only act if the goro is still
             * actually waiting for this I/O.  If the goro was already recycled
             * or reallocated, io_pending will be 0 and the CAS fails,
             * preventing stale CQEs from corrupting reused goro memory.
             *
             * If g == v->current, the goro called ui_uring_drain from within
             * ui_io_submit_and_wait's own completion check loop — it will see
             * io_pending==0 and return on its own.  Do NOT ui_wakeup the
             * current goroutine (that would double-schedule it). */
            ui_Goro *g = (ui_Goro *)(uintptr_t)cqe->user_data;
            if (__sync_bool_compare_and_swap(&g->io_pending, 1, 0)) {
                g->io_result = cqe->res;
                if (g != v->current) {
                    ui_wakeup(g);
                } else {
                    g->state = UI_READY;
                }
            }
            if (v->uring_pending > 0)
                v->uring_pending--;
        }
        else
        {
            /* Batch intermediate / user_data == 0 completions */
            if (v->uring_pending > 0)
                v->uring_pending--;
        }
skip:
        head++;
    }
    *v->cq_head = head;
    __sync_synchronize();
}

/* Helper: submit I/O op, yield, wait for completion */
static ssize_t
ui_io_submit_and_wait(ui_vCPU *v, struct io_uring_sqe *sqe)
{
    ui_Goro *cur = v->current;
    sqe->user_data = (uint64_t)(uintptr_t)cur;
    cur->io_pending = 1;
    ui_uring_submit(v);

    cur->io_token = (uint64_t)(uintptr_t)cur;

    /* Drain any immediate completion BEFORE setting state to WAITING.
     * If the I/O completed instantly (within the io_uring_enter syscall),
     * ui_uring_drain will CAS io_pending 1→0 and set io_result.
     * Return immediately — no state change needed, the goro is still running. */
    ui_uring_drain(v);
    if (!cur->io_pending) {
        /* Fast path: I/O completed inline, no yield.
         * Yield every N completions for fairness (prevent I/O busy-loop from
         * starving other goros in cooperative scheduling). */
        if (ui_yield_io_mask && (++v->io_count & ui_yield_io_mask) == 0)
            ui_Yield();
        return cur->io_result;
    }

    cur->state = UI_WAITING;

    while (cur->io_pending)
    {
        cur->state = UI_WAITING;
        /* Yield and wait for completion */
        ui_switch(&cur->rsp, v->sched_rsp);
        ui_uring_drain(v);
    }

    return cur->io_result;
}

/* ── I/O operations — io_uring path with POSIX fallback ── */

ssize_t
ui_Read(int fd, void *buf, size_t count)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return read(fd, buf, count);
    if (ui_vcpu_ensure_ring(v) < 0) return read(fd, buf, count);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return read(fd, buf, count);

    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->off = -1;
    return ui_io_submit_and_wait(v, sqe);
}

ssize_t
ui_Write(int fd, const void *buf, size_t count)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return write(fd, buf, count);
    if (ui_vcpu_ensure_ring(v) < 0) return write(fd, buf, count);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return write(fd, buf, count);

    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->off = -1;
    return ui_io_submit_and_wait(v, sqe);
}

int
ui_Open(const char *pathname, int flags, ...)
{
    va_list ap;
    mode_t mode = 0;
    if (flags & O_CREAT) { va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return open(pathname, flags, mode);
}

ssize_t
ui_Recv(int fd, void *buf, size_t count, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return recv(fd, buf, count, flags);
    if (ui_vcpu_ensure_ring(v) < 0) return recv(fd, buf, count, flags);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return recv(fd, buf, count, flags);

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->rw_flags = flags;
    return ui_io_submit_and_wait(v, sqe);
}

ssize_t
ui_Send(int fd, const void *buf, size_t count, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return send(fd, buf, count, flags);
    if (ui_vcpu_ensure_ring(v) < 0) return send(fd, buf, count, flags);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return send(fd, buf, count, flags);

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->rw_flags = flags;
    return ui_io_submit_and_wait(v, sqe);
}

int
ui_Connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return connect(fd, addr, addrlen);
    if (ui_vcpu_ensure_ring(v) < 0) return connect(fd, addr, addrlen);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return connect(fd, addr, addrlen);

    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)addr;
    sqe->off = addrlen;
    return (int)ui_io_submit_and_wait(v, sqe);
}

int
ui_Accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return accept(fd, addr, addrlen);
    if (ui_vcpu_ensure_ring(v) < 0) return accept(fd, addr, addrlen);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return accept(fd, addr, addrlen);

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)addr;
    sqe->off = (unsigned long)(uintptr_t)addrlen;
    return (int)ui_io_submit_and_wait(v, sqe);
}

int
ui_Close(int fd)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return close(fd);
    if (ui_vcpu_ensure_ring(v) < 0) return close(fd);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return close(fd);

    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd = fd;
    ssize_t ret = ui_io_submit_and_wait(v, sqe);
    return (ret < 0) ? (int)ret : 0;
}

int
ui_Shutdown(int fd, int how)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return shutdown(fd, how);
    if (ui_vcpu_ensure_ring(v) < 0) return shutdown(fd, how);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return shutdown(fd, how);

    sqe->opcode = IORING_OP_SHUTDOWN;
    sqe->fd = fd;
    sqe->rw_flags = (unsigned)how;
    return (int)ui_io_submit_and_wait(v, sqe);
}

ssize_t
ui_SendMsg(int fd, const struct msghdr *msg, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return sendmsg(fd, msg, flags);
    if (ui_vcpu_ensure_ring(v) < 0) return sendmsg(fd, msg, flags);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return sendmsg(fd, msg, flags);

    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)msg;
    sqe->len = 1;
    sqe->msg_flags = (unsigned)flags;
    return ui_io_submit_and_wait(v, sqe);
}

ssize_t
ui_RecvMsg(int fd, struct msghdr *msg, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return recvmsg(fd, msg, flags);
    if (ui_vcpu_ensure_ring(v) < 0) return recvmsg(fd, msg, flags);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return recvmsg(fd, msg, flags);

    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)msg;
    sqe->len = 1;
    sqe->msg_flags = (unsigned)flags;
    return ui_io_submit_and_wait(v, sqe);
}

int
ui_PollAdd(int fd, unsigned events)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return poll(&(struct pollfd){.fd = fd, .events = (short)events}, 1, -1);
    if (ui_vcpu_ensure_ring(v) < 0) return poll(&(struct pollfd){.fd = fd, .events = (short)events}, 1, -1);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return poll(&(struct pollfd){.fd = fd, .events = (short)events}, 1, -1);

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = events;
    return (int)ui_io_submit_and_wait(v, sqe);
}

/* ── Multishot recvmsg + provided buffer ring ── */

struct ui_RecvMulti *
ui_RecvMulti(int fd, ui_RecvMultiCb cb, void *ctx)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return NULL;
    if (ui_vcpu_ensure_ring(v) < 0) return NULL;
    if (ui_vcpu_ensure_buf_ring(v) < 0) return NULL;

    struct ui_RecvMulti *rm = (struct ui_RecvMulti *)calloc(1, sizeof(*rm));
    if (!rm) return NULL;

    rm->fd = fd;
    rm->active = 1;
    rm->cb = cb;
    rm->ctx = ctx;

    /* msghdr: kernel reads it during io_uring_enter, copies internally,
     * and never touches the original again for this multishot recv.
     * Source address is stored in the buffer ring via io_uring_recvmsg_out. */
    rm->msg.msg_name    = &rm->addr;
    rm->msg.msg_namelen = sizeof(rm->addr);
    rm->msg.msg_iov     = &rm->iov;
    rm->msg.msg_iovlen  = 1;
    rm->msg.msg_control = NULL;
    rm->msg.msg_controllen = 0;
    rm->msg_namelen    = sizeof(rm->addr);
    rm->msg_controllen = 0;

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe)
    {
        free(rm);
        return NULL;
    }

    sqe->opcode    = IORING_OP_RECVMSG;
    sqe->fd        = fd;
    sqe->addr      = (uint64_t)(uintptr_t)&rm->msg;
    sqe->len       = 1;
    sqe->msg_flags = 0;
    sqe->flags    |= IOSQE_BUFFER_SELECT;
    sqe->buf_index = (uint16_t)v->bgid;
    sqe->ioprio   |= IORING_RECV_MULTISHOT;
    sqe->user_data = (uint64_t)(uintptr_t)rm | 1;

    rm->next = v->active_multishot;
    v->active_multishot = rm;
    v->has_multishot = 1;

    ui_uring_submit(v);
    return rm;
}

void
ui_RecvMultiClose(struct ui_RecvMulti *rm)
{
    if (!rm || !rm->active) return;
    rm->active = 0;

    ui_vCPU *v = ui_this_vcpu;
    if (v)
    {
        for (struct ui_RecvMulti **p = &v->active_multishot; *p; p = &(*p)->next)
        {
            if (*p == rm)
            {
                *p = rm->next;
                break;
            }
        }
        if (!v->active_multishot)
            v->has_multishot = 0;

        /* Submit async cancel to stop the kernel side, then drain
         * so any residual CQEs are consumed before we free rm. */
        struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
        if (sqe)
        {
            sqe->opcode = IORING_OP_ASYNC_CANCEL;
            sqe->addr = (uint64_t)(uintptr_t)rm | 1;
            sqe->off = IORING_ASYNC_CANCEL_USERDATA;
            sqe->fd = -1;
            ui_uring_submit(v);
        }
        /* Drain any remaining CQEs referencing rm */
        ui_uring_drain(v);
    }
    free(rm);
}

/* ── Batch submission ── */
static void
ui_uring_submit_batch(ui_vCPU *v, unsigned count)
{
    unsigned tail = *v->sq_tail;
    unsigned mask = *v->sq_ring_mask;

    if (!v->no_sq_array) {
        for (unsigned i = 0; i < count; i++)
            v->sq_array[(tail + i) & mask] = (tail + i) & mask;
    }

    __sync_synchronize();

    *v->sq_tail = tail + count;
    v->uring_pending += count;

    ui_uring_enter(v->ring_fd, count, 0, IORING_ENTER_GETEVENTS);
}

int
ui_SendMMsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return (int)sendmmsg(fd, msgvec, vlen, flags);
    if (ui_vcpu_ensure_ring(v) < 0) return (int)sendmmsg(fd, msgvec, vlen, flags);
    if (vlen == 0) return 0;

    {
        unsigned head = *v->sq_head;
        unsigned used = *v->sq_tail - head;
        if (used + vlen > *v->sq_ring_entries) {
            ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
            ui_uring_drain(v);
        }
        head = *v->sq_head;
        if (*v->sq_tail - head + vlen > *v->sq_ring_entries)
            goto fallback;
    }

    ui_Goro *cur = v->current;
    unsigned tail = *v->sq_tail;
    unsigned mask = *v->sq_ring_mask;

    for (unsigned i = 0; i < vlen; i++) {
        struct io_uring_sqe *sqe = &v->sq_sqes[(tail + i) & mask];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_SENDMSG;
        sqe->fd = fd;
        sqe->addr = (unsigned long)(uintptr_t)&msgvec[i].msg_hdr;
        sqe->len = 1;
        sqe->msg_flags = (unsigned)flags;

        if (i + 1 < vlen) {
            sqe->flags |= IOSQE_IO_LINK;
            sqe->user_data = 0;
        } else {
            sqe->user_data = (uint64_t)(uintptr_t)cur;
        }
    }

    cur->io_pending = 1;
    ui_uring_submit_batch(v, vlen);

    cur->io_token = (uint64_t)(uintptr_t)cur;
    ui_uring_drain(v);
    if (!cur->io_pending)
        return (int)cur->io_result;

    cur->state = UI_WAITING;
    while (cur->io_pending) {
        cur->state = UI_WAITING;
        ui_switch(&cur->rsp, v->sched_rsp);
        ui_uring_drain(v);
    }

    return (int)cur->io_result;

fallback:
    {
        int total = 0;
        for (unsigned i = 0; i < vlen; i++) {
            ssize_t ret = ui_SendMsg(fd, &msgvec[i].msg_hdr, flags);
            if (ret < 0) return (int)ret;
            msgvec[i].msg_len = (unsigned int)ret;
            total++;
        }
        return total;
    }
}

int
ui_RecvMMsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags)
{
    ui_vCPU *v = ui_this_vcpu;
    if (!v || !v->current) return (int)recvmmsg(fd, msgvec, vlen, flags, NULL);
    if (ui_vcpu_ensure_ring(v) < 0) return (int)recvmmsg(fd, msgvec, vlen, flags, NULL);
    if (vlen == 0) return 0;

    {
        unsigned head = *v->sq_head;
        unsigned used = *v->sq_tail - head;
        if (used + vlen > *v->sq_ring_entries) {
            ui_uring_enter(v->ring_fd, 0, 0, IORING_ENTER_GETEVENTS);
            ui_uring_drain(v);
        }
        head = *v->sq_head;
        if (*v->sq_tail - head + vlen > *v->sq_ring_entries)
            goto fallback;
    }

    ui_Goro *cur = v->current;
    unsigned tail = *v->sq_tail;
    unsigned mask = *v->sq_ring_mask;

    for (unsigned i = 0; i < vlen; i++) {
        struct io_uring_sqe *sqe = &v->sq_sqes[(tail + i) & mask];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_RECVMSG;
        sqe->fd = fd;
        sqe->addr = (unsigned long)(uintptr_t)&msgvec[i].msg_hdr;
        sqe->len = 1;
        sqe->msg_flags = (unsigned)flags;

        if (i + 1 < vlen) {
            sqe->flags |= IOSQE_IO_LINK;
            sqe->user_data = 0;
        } else {
            sqe->user_data = (uint64_t)(uintptr_t)cur;
        }
    }

    cur->io_pending = 1;
    ui_uring_submit_batch(v, vlen);

    cur->io_token = (uint64_t)(uintptr_t)cur;
    ui_uring_drain(v);
    if (!cur->io_pending)
        return (int)cur->io_result;

    cur->state = UI_WAITING;
    while (cur->io_pending) {
        cur->state = UI_WAITING;
        ui_switch(&cur->rsp, v->sched_rsp);
        ui_uring_drain(v);
    }

    return (int)cur->io_result;

fallback:
    {
        int total = 0;
        for (unsigned i = 0; i < vlen; i++) {
            ssize_t ret = ui_RecvMsg(fd, &msgvec[i].msg_hdr, flags);
            if (ret < 0) break;
            msgvec[i].msg_len = (unsigned int)ret;
            total++;
        }
        return total;
    }
}
