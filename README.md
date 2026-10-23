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
- **事件驱动空载** — ring 通过 `IORING_REGISTER_EVENTFD` 把每个 completion 直接
  post 到本 vCPU 的 eventfd，一次 `ppoll(event_fd)` 即覆盖跨 vCPU 唤醒、I/O 完成、
  sleepq 到期三类唤醒源；空闲 vCPU 每次唤醒只花一次 syscall，不再有定频轮询
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
make test-idle-wait   # 事件驱动空载：唤醒延迟 + 关机不空等
make test-stack-overflow # 8MB 栈溢出干净报错
make bench-ui         # 协程调度基准
make bench-ui-io      # I/O 基准
make bench-stack      # 栈占用与回收策略基准
make bench-json       # 采集为 JSON（scripts/bench_collect.mjs）
make bench-stat       # 判定两份记录的差异是不是真的（scripts/benchstat.mjs）
```

基准驱动在 `tests/bench.{c,h}`，按 Go 的 `testing.B` 组织：每个基准声明自己
的 op 数（这是指标定义的一部分，不是调参旋钮），预热轮次被丢弃，测量重复多
次并保留每一个样本。报告给的是中位数加离散度，不是一个孤零零的 ns/op——
共享 runner 上单个数字是噪声，样本才是测量。

前端面板（`web/`，Solid 2 + TypeScript）本地用 bun：

```sh
cd web && bun install   # 安装依赖（本地用 bun）
cd web && bun run dev   # 开发服务器
cd web && bun run test  # 纯逻辑单元测试（vitest）
cd web && bun run build # 产出 web/dist
```

CI 里改用 node/npm（`@solidjs/web` 需要 node ≥22.12）。

默认 `TOOLCHAIN=clang`（musl 静态链接）。可用 `TOOLCHAIN=zig|gcc|gcc-musl`
切换；`BUILD=release` 为优化构建；`STD=c23|gnu23` 选择标准。

## 基准追踪

`.github/workflows/bench.yml` 在 main 每次推送、每日定时、手动触发，以及针对
main 的 PR 时构建 release 基准。基准进程绑在 runner 真正分给我们的 CPU 上
（vCPU 线程继承亲和性），每项指标测 6 次，两轮进程运行共 12 个样本。

每个 job 都做**同机 A/B**：同一个 runner 上分别编译并交替运行两个版本的
二进制，两边共担机器漂移，因此对比结果不是机器差异造成的。基线用的是**同一
份基准驱动**（旧版本的仓库里没有它），否则基线每个指标只有一个样本，比较给
不出显著性。对照关系按事件如下：

| 触发 | 对照 base | 新版本 head |
|------|-----------|-------------|
| PR（针对 main） | `pull_request.base.sha`（合并基点） | PR head |
| 推送到 main | `github.event.before`（上一个 main tip） | 新提交 |
| 定时 / 手动 | 无（手动可用 `base_ref` 指定） | 目标 ref |

每个 job 结束时跑 `scripts/benchstat.mjs`，用非参数的 Mann-Whitney U 检验
（和 Go 的 benchstat 同一个方法）判断两边的差异是不是比噪声大：判定结果写进
job summary，PR 页面上直接能看到哪些指标真的变了、变了多少、p 值多少，哪些
只是噪声。同时用 `strace -c` 采集每个基准的系统调用次数——延迟在共享 runner
上是噪声，系统调用次数不是，热路径上多一次 `madvise` 会显示成一个确切的数字。

结果追加到 `gh-pages` 分支的历史数据，并发布可视化面板：

**https://apeiroha.github.io/ui/**

面板是 `web/` 下的 Solid 2 + TypeScript 应用（Vite 构建，图表为自绘内联
SVG，无外部图表库）。追踪页展示指标随时间的变化趋势，头条数字用中位数，图上
画出 p50–p95 的带（带宽就是不稳定），每个指标标出样本的变异系数——CV > 10%
的指标旁边的百分比是没有意义的。对比页提供 latest、time-aligned、same-machine
pair 三种模式，最后一种基于记录里内嵌的 baseline 和预先算好的显著性判定。
历史数据存于 `gh-pages` 的 `data/bench.json`，页面由 `bench_publish.sh` 从构建
产物 `web/dist/` 发布。

## 环境变量

| 变量 | 作用 | 默认 |
|------|------|------|
| `UI_NVCPUS` | vCPU 数量（1–64） | CPU 核数 |
| `UI_YIELD_IO_MASK` | 连续 I/O 完成每 N 次自愿让出，0 关闭 | 255 |
| `UI_IDLE_WAIT_MS` | 空闲 vCPU 的兜底唤醒超时（ms）。所有真实唤醒源都是事件驱动的，这个值只兜底“丢唤醒”，并直接决定空载 CPU | 100 |
| `UI_STACK_TRIM` | 回收 goro 时栈页的归还方式：0 保留（**默认**，最快，RSS 按池上限增长）、1 `MADV_FREE`（延迟归还，内核在内存压力下回收）、2 `MADV_DONTNEED`（立即归还，RSS 最低） | 0 |
| `UI_STACK_MEMORY_LIMIT` | goro 池可占用的栈内存上限（字节）。0 = 不限（速度优先）；非 0 = 内存优先：自动切到 `MADV_DONTNEED` 并给池封顶，超预算的栈直接归还内核。**只约束池，不含存活 goro 的栈** | 0 |

> `UI_STACK_RELEASE_ON_RECYCLE` 已废弃：它从来只被读取、从未被使用，设置它没有任何效果。
> 现在用 `UI_STACK_TRIM` 选择实际的 madvise 策略。

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

spawn 失败语义：

`ui_Go()` 及其变体在**无法创建 goro 时直接终止进程**（goro 槽位或地址空间耗尽）——
静默丢弃一个任务对调用方的工作负载是正确性 bug。如果调用方能降级处理，用
`ui_TryGo()` / `ui_TryGo1()`：成功返回 0 并回填 goro id，失败返回 -1 且 `errno` 被置为 `ENOMEM`。

并发上限：每个 goro 栈约占 2 个 VMA（arena 为 `PROT_NONE`，每槽一个 guard 页把它切成两段），
所以并发 goro 数受内核 `vm.max_map_count` 限制（默认 1048576，即约 52 万个并发 goro）。
需要更多时调高该值：`sysctl vm.max_map_count=4194304`。

栈内存策略（速度 vs 内存）：

```c
ui_SetStackTrimPolicy(UI_STACK_TRIM_NONE);       /* 默认：保留页，回收最快 */
ui_SetStackMemoryLimit(4 * 1024 * 1024);         /* 需要内存上限时用这个 */
```

回收 goro 的代价主要不是 TLB shootdown，而是 `madvise` 这个系统调用本身。实测
（4 vCPU，稳态 spawn 40 万次）：

| 配置 | ns/op | 常驻内存 |
|---|---|---|
| `NONE`（默认，保留页） | 298 | 23.8 MB |
| `MADV_FREE` | 1994 | 18.4 MB |
| `MADV_DONTNEED` | 3022 | 2.6 MB |
| `NONE` + 4MB 预算 | 866 | 5.7 MB |
| `MADV_DONTNEED` + 4MB 预算 | 2927 | 1.6 MB |

（400k 次稳态 spawn，3 次取中位数。）

`MADV_FREE` 确实能少占约 5MB，但代价是 6.5 倍的延迟。**要低常驻内存，
预算比策略更划算**：`NONE` + 4MB 预算比 `MADV_FREE` **又快 2.3 倍又省
3 倍**——真正把页还回去的是"丢弃池里的栈"，不是 madvise 的 advice。
`MADV_FREE` 只在"无 swap 的容器里、池又必须保持很大"这一种情况下才合适。

预算只约束**池**里空闲 goro 的栈；存活 goro 的栈不在预算内，且估算偏宽松，
当作软上限使用。

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
