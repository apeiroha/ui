# ui — 并发/协程库 (うい)

基于 io_uring 的 M:N 协程运行时，C23 编写，零第三方运行时依赖（仅系统头：
pthread、io_uring、mmap、poll、eventfd、signal）。支持 work-stealing 多 vCPU
调度、可增长栈、channel、select、定时器、锁、异步 I/O 与 multishot recv。

## 特性

- **M:N 调度** — 64 个 vCPU、每 vCPU 独立 runq + 跨 vCPU work-stealing
- **可增长栈** — Go 式倍增（128KB 起步，8MB 保留 + guard page，越界干净报错）
- **同步原语** — channel（带缓存/直通交接）、select、mutex、cond、读写锁
- **异步 I/O** — io_uring 驱动的 Read/Write/Recv/Send/Accept/Connect，
  multishot recvmsg + buffer ring、mmsg 批量收发
- **零依赖** — 不需要任何外部库，`ui_Init` 后即可使用

## 构建

```sh
make libui          # 产出 build/libui.a
make test           # 全部测试
make test-ui        # 核心测试
make bench-ui       # 协程调度基准
make bench-ui-io    # I/O 基准
make test-p0-asan   # ASan 回归
```

默认 `TOOLCHAIN=clang`（musl 静态链接）。可用 `TOOLCHAIN=zig|gcc|gcc-musl`
切换；`BUILD=release` 为优化构建。

## 快速上手

```c
#include "ui.h"
#include <stdio.h>

static void worker(void) {
    printf("hello from goro %lu\n", (unsigned long)ui_now_us());
}

int main(void) {
    if (ui_Init() != 0) return 1;
    for (int i = 0; i < 1000; i++) ui_Go(worker);
    ui_Run();   /* 阻塞直到所有 goro 结束 */
    ui_Fini();
    return 0;
}
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
ui_core.c      调度器、Init/Fini/Run/Go/Yield/Sleep、io_uring 环管理
ui_stack.c     可增长栈（mmap + guard page）
ui_chan.c      channel、select、timer
ui_sync.c      mutex、cond、rwlock
ui_io.c        io_uring 异步 I/O、multishot recvmsg、mmsg 批量
ui_waitq.c     wait queue 抽象
ui_switch.S    x86-64 上下文切换
start.c        goro 入口 trampoline
ui.iroha       iroha 语言绑定
tests/         测试与基准
```