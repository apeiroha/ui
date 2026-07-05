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

uint64_t ui_Go(ui_Func0 f);
uint64_t ui_GoSized(ui_Func0 f, int stack_size);
uint64_t ui_Go1(void *fn, uintptr_t arg);
uint64_t ui_Go1Sized(void *fn, uintptr_t arg, int stack_size);

void     ui_Yield(void);
void     ui_Sleep(unsigned int ms);
void     ui_SleepUs(unsigned int us);

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

ssize_t  ui_Read(int fd, void *buf, size_t count);
ssize_t  ui_Write(int fd, const void *buf, size_t count);
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

#endif
