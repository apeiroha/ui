# 为什么 PhotonLibOS 没有 UI 的 work-stealing runq race

## 问题回顾

UI 的 `test_many_goros` (1000 协程并发创建并退出) 在多核下间歇性 SIGSEGV。根因：`ui_steal_work` 在不持有目标 vCPU 的 `runq_lock` 的情况下直接 splice 批处理到目标 runq。同时，目标 vCPU 的调度器可能在无锁情况下读取 `v->runq_sentinel.next`。两者形成 data race，导致 runq 链表被破坏。

## Photon 的架构差异

Photon 通过**四个设计决策**从根本上消除了这类 race：

### 1. Runq 是所有线程的集合，包括正在运行的线程

| | UI | Photon |
|---|---|---|
| 正在运行的 goro 是否在 runq 中 | **是** | **是** |
| 调度器如何选下一个 | 从 sentinel 取 head / 或 `current->next` | 旋转 `CURRENT` 指针到 `current->next()` |
| runq 能否为空 | 能 (sentinel 自指) | **不能** — `idle_worker` 始终在列表中 |

Photon 的 runq 是**循环双向链表**，包含该 vCPU 上的**所有**线程（包括空闲 worker）。调度只是纯指针操作——`CURRENT = CURRENT->next()`。不需要「取出 → 运行 → 完成后放回」的过程。这意味着**不存在「goroutine 不在 runq 中」的时间窗口**，因此也不存在「waker 插入时 goroutine 不在 runq 中，但 scheduler 以为它在」的竞态。

### 2. 异步锁 (asymmetric_spinLock)：前台永远不为后台自旋

```cpp
// Photon: asymmetric_spinLock (thread/thread.cpp:480-525)
class asymmetric_spinLock {
    atomic_bool foreground_locked;  // 被 vCPU 自己的调度器使用
    atomic_bool background_locked;  // 被 work-stealing 使用

    void foreground_lock() {
        foreground_locked = true;       // 简单 store，无 CAS
        while (background_locked)       // 等待后台释放（前台永远不会为后台旋转）
            spin_wait();
    }
    bool background_try_lock() {
        while (foreground_locked) spin_wait();  // 等待前台释放
        if (background_locked.exchange(true))    // CAS — 付费操作
            return false;                        // 失败，不等待
        if (foreground_locked) {                 // 前台在我们 CAS 后又抢占了！
            background_locked = false;           // 放弃，重试
            return false;
        }
        return true;
    }
};
```

关键特性：
- **前台锁** (vCPU 自己的调度器)：`foreground_lock()` 使用 **普通 store**（无 CAS，无总线锁），写入后检查 `background_locked`。前台**永远不会**为后台旋转。
- **后台锁** (work-stealing)：`background_try_lock()` 使用 **CAS**，如果前台持有锁则立即返回 false（**非阻塞**）。

对比 UI 的 `pthread_spinlock_t`：
- UI 的 `runq_lock` 是普通 spinlock，**没有前台/后台区分**
- work-stealing 获取锁时用的是 `pthread_spin_trylock` (非阻塞)
- **但修改目标 runq 时完全不拿锁**，直接写 `v->runq_sentinel` 的 `prev/next`

### 3. Standbyq：跨 vCPU 操作不直接接触 runq

Photon 中，**任何跨 vCPU 的 goroutine 迁移都不会直接修改目标 vCPU 的 runq**。而是通过 `standbyq`：

```
跨 vCPU 迁移 goroutine 的路径：
  线程 B 在 vCPU 0 上 → thread_interrupt → 
    如果 B 在另一 vCPU 上  → prelocked_thread_interrupt →
      th->dequeue_ready_atomic(STANDBY)        ← 从源 runq 摘除
      vcpu->move_to_standbyq_atomic(th)        ← 推入目标 standbyq（有独立 spinlock）
```

然后由目标 vCPU **自己**在 `resume_threads_inlined()` 中 drain `standbyq` 并插入 `runq`（在 `foreground_lock` 下）：

```cpp
// thread/thread.cpp:1253-1294
int resume_threads_inlined(vcpu_t* vcpu, const RunQ& runq) {
    // 原子地取出整个 standbyq
    list = standbyq.eject_whole_atomic();
    for (auto th : list) {
        th->state = READY;
        // ...
    }
    AtomicRunQ(runq).insert_list_before(list);  // foreground_lock 下插入 runq
}
```

**好处**：runq 永远只被其 owner vCPU 修改。work-stealing 从源 runq 摘除线程后，不会直接写入目标 runq，而是推入 standbyq。

对比 UI：
- `ui_steal_work` 从源 runq 摘除批处理后，**直接写入目标 runq** (`s->prev->next = batch_start; s->prev = batch_end`)，且**不持有目标 runq_lock**

### 4. 每线程独立 spinlock + RUNNING 状态守卫

Photon 在 `ws_scan_q` 中遍历每个线程时，会先锁该线程自己的 `th->lock`（而非 runq 全局锁）：

```cpp
// thread/thread.cpp:1991-2014
thread* ws_scan_q(...) {
    while (th != first) {
        SCOPED_LOCK(th->lock);    // ← 锁单个线程
        if (th->state == RUNNING || !th->allow_work_stealing()) {
            th = th->next();      // 跳过
        } else {
            auto next = th->remove_from_list();  // 从 victim's runq 摘除
            // ...
        }
    }
}
```

即使前台正好在操作这个线程，后台也不会偷到 RUNNING 状态的线程。

## 总结对比

| 维度 | UI (うい) | PhotonLibOS |
|------|-----------|-------------|
| Runq 锁类型 | `pthread_spinlock_t` | `asymmetric_spinLock` (前台/后台分离) |
| 前台获取开销 | CAS (有总线锁) | 普通 store (无 CAS) |
| 后台获取 | `pthread_spin_trylock` (CAS) | `background_try_lock` (CAS + 非阻塞) |
| 修改目标 runq 时 | **不加目标锁** | 通过 `standbyq`（有独立锁），由 owner 自行 drain |
| Work stealing 遍历时 | 只持源 runq_lock | 持源 runq_lock + 每个线程独立 spinlock |
| 正在运行的 goro | 在 runq 中 | 在 runq 中（无法被偷——`state==RUNNING` 跳过） |
| 跨 vCPU 同步 | 直接写 eventfd + runq | `standbyq`（有锁队列）→ drain 时插入 runq |
| 删除 runq 条目的时机 | cleanup 阶段（`ui_schedule` 中） | 仅在线程 sleep/done 时（`remove_current`） |

**结论**：Photon 的 work-stealing 之所以没有 UI 的 race，不是因为实现更复杂，而是因为**架构设计从根本上将「跨 vCPU 操作」与「runq 直接修改」解耦**。所有跨 vCPU 操作都走 `standbyq`（由目标 vCPU 自己 drain），而 runq 的 owner 使用轻量的 `asymmetric_spinLock` 确保前台不因后台而延迟。UI 要实现同样的安全性，需要：

1. 加 `standbyq`：work-stealing 和 waker 跨 vCPU 插入都走 standbyq，不直接接触 target runq
2. 换 `asymmetric_spinLock`：前台用普通 store（无 CAS），后台用非阻塞 CAS
3. 每线程加独立 spinlock：work-stealing 逐个锁定线程，而非依赖全局 runq_lock

## 2026-05-31 更新: 最终根因

### 真正的根因不是 standbyq 或 runq 锁

经过多轮调试，最终的 crash 根因是：

**`ui_vcpu_main` 没有设置 `v->thread`**

`ui_get_vcpu()` 通过扫描 `g_ui_sched.vcpus[]` 并调用 `pthread_equal()` 来定位当前 vCPU。但 `v->thread` 只在 `ui_Run` 中为 **vCPU 0** 设置，`pthread_create` 创建的其他 vCPU 线程的 `v->thread` 一直保持 0（`calloc` 初始化值）。

后果：
- 非 vCPU 0 的线程调用 `ui_get_vcpu()` → 找不到匹配 → **返回 NULL**
- goroutine 在这些 vCPU 上无法 yield、无法 block、无法 sleep
- `ui_goro_exit()` → `ui_get_vcpu()` 返回 NULL → `v->sched_rsp` 尚未初始化 → `ui_switch` 加载 NULL RSP → **SIGSEGV**
- `ui_get_vcpu_id()` 返回 0（默认值）→ `ui_runq_remove()` 使用错误 vCPU 的锁 → runq 链表损坏

### 修复

```c
static void *ui_vcpu_main(void *arg) {
    ui_vCPU *v = arg;
    v->thread = pthread_self();   // ← 关键：设置 thread ID
    ui_this_vcpu = v;             // ← __thread 变量实现 O(1) 查找
    ...
}
```

加上 `__thread ui_vCPU *ui_this_vcpu` 后，所有 vCPU 查找变为 O(1) 的 TLS 读取。

### 关于 Photon

Photon **没有这个问题**是因为它在 `vcpu_t` 的构造函数中立即设置 `thread_id`：
```cpp
vcpu->thread_id = pthread_self();
```

(见 `thread/thread.cpp` 中 vCPU 初始化代码)

UI 的 `ui_Init` 无法设置 vCPU 线程的 ID（因为 `pthread_create` 还没调用），而在 `ui_Run` 中只设置了 vCPU 0。Photon 的工作池初始化器会为每个 worker 线程设置 `thread_id`，包括主线程。
