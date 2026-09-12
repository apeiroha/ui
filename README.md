# ui — 并发/协程库 (うい)

基于 io_uring 的 M:N 协程运行时，C23 编写，零第三方运行时依赖（仅系统头：
pthread、io_uring、mmap、poll、eventfd、signal）。支持 work-stealing 多 vCPU
调度、LIFO 槽、可增长栈、channel、select、定时器、锁、异步 I/O、multishot
recvmsg 与 mmsg 批量收发。

## 特性

- **M:N 调度** — 最多 64 个 vCPU，每 vCPU 独立 FIFO runq + **LIFO 槽
  (runnext)**；跨 vCPU work-stealing，迁移经 `standbyq` 解耦（仅属主 drain）
- **定位运行** — `ui_GoOn*` 软绑定到指定 vCPU，`ui_PinTo` 运行时重绑定
- **可增长栈** — Go 式倍增（128KB 起步，8MB 保留 + guard page，越界干净报错），
  arena 槽复用避免反复 `mmap/mprotect`
- **同步原语** — channel（带缓存 / 无缓存直通交接）、select、mutex、cond、
  **按 vCPU 分片的读写锁**（读路径无总线锁竞争）
- **异步 I/O** — io_uring 驱动的 Read/Write/Open/Recv/Send/SendMsg/RecvMsg/
  PollAdd/Connect/Accept/Close/Shutdown；multishot recvmsg + buffer ring；
  mmsg 批量收发（SendMMsg/RecvMMsg/RecvBatch）
- **CQE 代际令牌** — 每次 I/O 提交自增 `io_token`，区分同地址前后代 completion，
  关闭 stale-CQE ABA 窗口
- **零依赖** — 不需要任何外部库，`ui_Init` 后即可使用

## 构建

```sh
make libui            # 产出 build/libui.a
make test             # 全部测试
make test-ui          # 核心功能回归
make test-c8-race     # 调度唤醒竞态
make test-p0          # P0 缺陷确认（send-closed / TimerStop / RecvMulti UAF）
make test-p0-asan     # 上述的 ASan 版本
make test-udp-echo    # UDP 回显（io_uring）
make test-sched-iopark# I/O 阻塞调度
make test-spawn-steal # 跨 vCPU 窃取
make test-lifo-slot   # LIFO 槽收敛 + 反垄断
make test-stack-overflow # 8MB 栈溢出干净报错
make bench-ui         # 协程调度基准
make bench-ui-io      # I/O 基准
```

默认 `TOOLCHAIN=clang`（musl 静态链接）。可用 `TOOLCHAIN=zig|gcc|gcc-musl`
切换；`BUILD=release` 为优化构建；`STD=c23|gnu23` 选择标准。

## 环境变量

| 变量 | 作用 | 默认 |
|------|------|------|
| `UI_NVCPUS` | vCPU 数量（1–64） | CPU 核数 |
| `UI_YIELD_IO_MASK` | 连续 I/O 完成每 N 次自愿让出，0 关闭 | 255 |
| `UI_STACK_RELEASE_ON_RECYCLE` | 回收 goro 时释放栈（不等 `ui_Fini`） | 0 |

## 快速上手

```c
#include "ui.h"
#include <stdio.h>

static void worker(void) {
    puts("hello from goro");
}

int main(void) {
    if (ui_Init() != 0) return 1;
    for (int i = 0; i < 1000; i++) ui_Go(worker);
    ui_Run();   /* 阻塞直到所有 goro 结束 */
    ui_Fini();
    return 0;
}
```

channel 与 select：

```c
uint64_t ch = ui_NewChan(sizeof(int), 0);   /* 0 = 无缓存（直通交接） */
ui_ChanSend(ch, &value);
ui_ChanRecv(ch, &value);
ui_ChanClose(ch);

/* 多路等待，返回就绪的 recv 索引；timeout_ms < 0 表示永久等待 */
uint64_t chs[2] = { ch_a, ch_b };
int vals[2] = {0, 0};
void *bufs[2] = { &vals[0], &vals[1] };
int idx = ui_SelectWait(chs, bufs, NULL, NULL, 2, 0, 100);
```

异步 I/O（当前 goro 阻塞、底层不阻塞 vCPU）：

```c
ssize_t n = ui_Read(fd, buf, sizeof buf);
int     c = ui_Accept(listen_fd, &addr, &addrlen);
```

## iroha 绑定

`ui.iroha` 是 iroha 语言的 FFI 绑定（纯 extern 声明，对应本库的 C ABI）。
生成的 `ui.iroha.c` 为编译器产物（已 gitignore），用 `iroha -c ui.iroha`
按需生成。本仓库只维护源码，不包含生成产物。

## 设计参考

`report_photon_vs_ui_workstealing.md` 记录了与 Photon 的 work-stealing 竞态
对比及调度器演进历程。

## 布局

```
ui.c           入口/聚合
ui_core.c      调度器、Init/Fini/Run/Go/GoOn/Yield/Sleep/PinTo、standbyq、
               runnext LIFO 槽、work-stealing、io_uring 环管理
ui_stack.c     可增长栈（mmap + guard page + arena 复用）
ui_chan.c      channel、select、timer
ui_sync.c      mutex、cond、分片 rwlock
ui_io.c        io_uring 异步 I/O、multishot recvmsg、mmsg 批量
ui_waitq.c     wait queue 抽象
ui_switch.S    x86-64 上下文切换
start.c        vCPU 线程启动 / goro trampoline
ui.iroha       iroha 语言绑定
ui.h           公共 C ABI
ui_internal.h  内部结构（ui_Goro / ui_vCPU / ui_Sched）
tests/         测试与基准
```
