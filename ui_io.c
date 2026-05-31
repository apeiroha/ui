#include "ui_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

#ifndef IORING_FEAT_NO_SQARRAY
#define IORING_FEAT_NO_SQARRAY (1U << 10)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 12)
#endif

#define UI_URING_ENTRIES 1024
#define UI_IO_TIMEOUT_MS 5000

static int
ui_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int
ui_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
               unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

int
ui_vcpu_ensure_ring(ui_vCPU *v)
{
    if (v->ring_fd > 0)
        return 0;

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER
            | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_NO_SQARRAY;

    v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    if (v->ring_fd < 0)
    {
        p.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_NO_SQARRAY;
        v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    }
    if (v->ring_fd < 0)
    {
        p.flags = IORING_SETUP_COOP_TASKRUN;
        v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    }
    if (v->ring_fd < 0)
    {
        p.flags = 0;
        v->ring_fd = ui_uring_setup(UI_URING_ENTRIES, &p);
    }
    if (v->ring_fd < 0)
        return -1;

    v->no_sq_array = (p.features & IORING_FEAT_NO_SQARRAY) != 0;

    size_t sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

    void *sq_ptr = mmap(0, sq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, v->ring_fd,
                        IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) goto err;

    void *cq_ptr = mmap(0, cq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, v->ring_fd,
                        IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) goto err;

    v->sq_sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, v->ring_fd,
                      IORING_OFF_SQES);
    if (v->sq_sqes == MAP_FAILED) goto err;

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
    if (v->ring_fd >= 0) close(v->ring_fd);
    v->ring_fd = 0;
    return -1;
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

    /* Submit via single syscall */
    ui_uring_enter(v->ring_fd, 1, 0, 0);
}

/* Drain all available CQEs, waking goroutines */
static void
ui_uring_drain(ui_vCPU *v)
{
    unsigned head = *v->cq_head;
    unsigned tail = *v->cq_tail;
    unsigned mask = *v->cq_ring_mask;

    while (head != tail)
    {
        struct io_uring_cqe *cqe = &v->cq_cqes[head & mask];
        if (cqe->user_data != 0)
        {
            ui_Goro *g = (ui_Goro *)(uintptr_t)cqe->user_data;
            g->io_result = cqe->res;
            g->state = UI_READY;
            ui_runq_insert(v, g);
            v->uring_pending--;
        }
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
    ui_uring_submit(v);

    cur->state = UI_WAITING;
    cur->io_token = (uint64_t)(uintptr_t)cur;

    /* Check if completion already arrived */
    ui_uring_drain(v);

    if (cur->state == UI_WAITING)
    {
        /* io_uring completion not yet available.
         * Use ui_Yield to properly save g->rsp and hand control
         * to the scheduler. The idle loop will drain CQEs and
         * insert us back into the runq when the CQE arrives.
         * ui_Yield sets state=READY, so we compensate.
         * This may schedule us a few times before the CQE arrives,
         * but it correctly preserves the goroutine's stack. */
        for (;;)
        {
            cur->state = UI_READY;
            ui_Yield();
            cur->state = UI_WAITING;
            ui_uring_drain(v);
            if (cur->state != UI_WAITING)
                break;
        }
    }


    return cur->io_result;
}

ssize_t
ui_Read(int fd, void *buf, size_t count)
{
    ui_vCPU *v = NULL;
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        if (pthread_equal(g_ui_sched.vcpus[i].thread, pthread_self()))
        { v = &g_ui_sched.vcpus[i]; break; }
    }

    if (!v || !v->current) return read(fd, buf, count);
    if (ui_vcpu_ensure_ring(v) < 0) return read(fd, buf, count);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return read(fd, buf, count);

    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->off = 0;
    return ui_io_submit_and_wait(v, sqe);
}

ssize_t
ui_Write(int fd, const void *buf, size_t count)
{
    ui_vCPU *v = NULL;
    for (int i = 0; i < g_ui_sched.nvcpus; i++)
    {
        if (pthread_equal(g_ui_sched.vcpus[i].thread, pthread_self()))
        { v = &g_ui_sched.vcpus[i]; break; }
    }

    if (!v || !v->current) return write(fd, buf, count);
    if (ui_vcpu_ensure_ring(v) < 0) return write(fd, buf, count);

    struct io_uring_sqe *sqe = ui_uring_get_sqe(v);
    if (!sqe) return write(fd, buf, count);

    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (unsigned long)(uintptr_t)buf;
    sqe->len = count;
    sqe->off = 0;
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
