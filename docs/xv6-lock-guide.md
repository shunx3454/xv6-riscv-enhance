# xv6-riscv 锁与 SMP 同步机制

本文总结 xv6-riscv 在多 hart/SMP 环境下的同步措施，重点分析 spinlock、sleeplock、sleep/wakeup、关中断、原子操作、内存顺序以及典型使用场景。

相关源码：

- [`kernel/spinlock.h`](../kernel/spinlock.h)：自旋锁结构定义。
- [`kernel/spinlock.c`](../kernel/spinlock.c)：自旋锁、关中断嵌套、内存顺序实现。
- [`kernel/sleeplock.h`](../kernel/sleeplock.h)：睡眠锁结构定义。
- [`kernel/sleeplock.c`](../kernel/sleeplock.c)：睡眠锁实现。
- [`kernel/proc.c`](../kernel/proc.c)：进程锁、调度、`sleep_prepare()`、`sleep()`、`wakeup()`。
- [`kernel/proc.h`](../kernel/proc.h)：`struct cpu`、`struct proc`。
- [`kernel/main.c`](../kernel/main.c)：多 hart 启动同步。
- [`kernel/riscv.h`](../kernel/riscv.h)：中断开关、CSR 访问。

## 1. 为什么 xv6 需要锁

xv6 默认可以用多个 hart 启动，例如 QEMU 参数：

```bash
-smp 3
```

这意味着多个 CPU/hart 可以同时运行内核代码。如果没有同步，多个 hart 可能同时修改共享数据：

```text
空闲物理页链表
进程表
buffer cache
inode cache
file table
pipe buffer
ticks
log 状态
```

典型错误包括：

```text
两个 hart 同时分配同一个物理页
两个 hart 同时运行同一个进程
一个 hart 正在修改 inode，另一个 hart 同时读写同一 inode
一个进程刚准备睡眠，另一个进程已经 wakeup，导致 lost wakeup
```

xv6 的同步机制可以概括为：

```text
spinlock + 原子指令 + acquire/release 内存顺序
push_off/pop_off 防止本 hart 中断重入死锁
sleeplock + sleep/wakeup 支持长时间等待
per-CPU / per-proc lock 降低共享和竞态
启动阶段 fence + started 同步多个 hart
```

## 2. spinlock 数据结构

定义在 [`kernel/spinlock.h`](../kernel/spinlock.h)：

```c
struct spinlock {
  uint locked; // Is the lock held?

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.
};
```

字段含义：

| 字段 | 作用 |
|---|---|
| `locked` | 锁状态，0 表示空闲，1 表示已持有 |
| `name` | 锁名，调试和 panic 输出用 |
| `cpu` | 当前持有锁的 CPU，用于 `holding()` 和调试 |

初始化在 [`kernel/spinlock.c`](../kernel/spinlock.c)：

```c
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}
```

## 3. acquire：获取自旋锁

核心实现：

```c
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if (holding(lk))
    panic("acquire");

  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)
    ;

  lk->cpu = mycpu();
}
```

执行步骤：

```text
1. push_off() 关闭当前 hart 中断
2. 检查当前 CPU 是否已经持有该锁，防止重复加锁
3. 原子交换 locked，尝试把 locked 从 0 改成 1
4. 如果原值不是 0，说明锁被别人持有，继续自旋
5. 成功后记录持锁 CPU
```

这里使用：

```c
__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE)
```

在 RISC-V 上会生成原子交换类指令，例如 `amoswap.w.aq`。`aq` / acquire 语义保证：

```text
成功获取锁之后，临界区中的 load/store 不能被重排到 acquire 之前。
```

因此 spinlock 不只是“互斥”，还提供必要的内存顺序约束。

## 4. release：释放自旋锁

核心实现：

```c
void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);

  pop_off();
}
```

执行步骤：

```text
1. 检查当前 CPU 确实持有该锁
2. 清空调试字段 lk->cpu
3. 用 release 语义把 locked 置 0
4. pop_off() 恢复当前 hart 的中断状态
```

这里使用：

```c
__atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE)
```

release 语义保证：

```text
临界区内的 load/store 不能被重排到 release 之后；
其他 CPU 获得锁后，应该能看到释放锁前的临界区写入。
```

在 RISC-V 上，这通常会生成类似：

```asm
fence rw,w
sw zero,0(...)
```

这就是锁的内存可见性基础。

## 5. acquire/release 与 `__sync_synchronize`

较早的 xv6 版本常见写法是：

```c
__sync_lock_test_and_set(...)
__sync_synchronize()
__sync_lock_release(...)
```

本仓库当前使用的是更新的 `__atomic_*` 接口：

```c
__atomic_exchange_n(..., __ATOMIC_ACQUIRE)
__atomic_store_n(..., __ATOMIC_RELEASE)
```

二者目的相同：保证临界区内存访问不会越过锁边界。

区别可以概括为：

| 写法 | 语义 |
|---|---|
| `__sync_synchronize()` | 全内存屏障，比较强 |
| `__ATOMIC_ACQUIRE` | 获取锁后的读写不能提前到锁前 |
| `__ATOMIC_RELEASE` | 释放锁前的读写不能延后到锁后 |

对锁来说，acquire/release 语义已经足够表达：

```text
lock acquire 之后才能访问临界区数据
临界区写入必须在 lock release 前对后续持锁者可见
```

## 6. 为什么 acquire 前要关中断

`acquire()` 开头调用：

```c
push_off();
```

目的是防止同一 hart 上的中断重入导致死锁。

如果不关中断，可能发生：

```text
hart0 的普通内核代码持有 lock
  -> hart0 发生中断
  -> 中断处理函数也 acquire(lock)
  -> hart0 自旋等待自己释放 lock
  -> 死锁
```

关闭中断只影响当前 hart，不会阻止其他 hart 运行；跨 hart 的互斥仍然由原子锁变量提供。

## 7. push_off / pop_off：嵌套关中断

实现位于 [`kernel/spinlock.c`](../kernel/spinlock.c)。

`push_off()`：

```c
void
push_off(void)
{
  uint64 flags = rc_sstatus(SSTATUS_SIE);
  int old = !!(flags & SSTATUS_SIE);

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}
```

`pop_off()`：

```c
void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}
```

每个 CPU 有：

```c
int noff;
int intena;
```

含义：

| 字段 | 作用 |
|---|---|
| `noff` | 当前 CPU 关中断嵌套层数 |
| `intena` | 最外层 `push_off()` 前中断是否开启 |

为什么要嵌套？

```text
acquire(lock1)
  acquire(lock2)
  release(lock2)
release(lock1)
```

如果 `release(lock2)` 时直接打开中断，就会破坏 `lock1` 持有期间的中断关闭约束。因此 xv6 用 `noff` 确保只有最外层 `pop_off()` 才恢复中断。

## 8. holding：判断当前 CPU 是否持锁

```c
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}
```

注意这里判断的是：

```text
当前 CPU 是否持有锁
```

不是“当前进程是否持有锁”。这是因为 spinlock 是短临界区的 CPU 级同步工具，且持有 spinlock 时通常关闭了当前 CPU 中断，不应发生普通进程切换。

## 9. sleeplock 数据结构

定义在 [`kernel/sleeplock.h`](../kernel/sleeplock.h)：

```c
struct sleeplock {
  uint locked;        // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock

  // For debugging:
  char *name; // Name of lock.
  int pid;    // Process holding lock
};
```

字段含义：

| 字段 | 作用 |
|---|---|
| `locked` | 睡眠锁是否被持有 |
| `lk` | 保护 sleeplock 自身状态的 spinlock |
| `name` | 调试用锁名 |
| `pid` | 当前持有该 sleeplock 的进程 pid |

sleeplock 适合等待时间可能较长的资源，例如：

```text
inode 内容
buffer 数据块
磁盘 IO 相关资源
```

## 10. acquiresleep：获取睡眠锁

当前仓库实现：

```c
void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while (lk->locked) {
    sleep_prepare(lk);
    release(&lk->lk);
    sleep();
    acquire(&lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}
```

流程：

```text
1. 先拿内部 spinlock，保护 sleeplock 状态
2. 如果 locked == 1，说明资源被其他进程持有
3. 调用 sleep_prepare(lk)，登记当前进程等待的 channel
4. 释放内部 spinlock
5. 调用 sleep() 阻塞当前进程
6. 被 wakeup 后重新获取内部 spinlock
7. 再次检查 locked，条件满足才真正持有 sleeplock
```

这里的 `while` 不能换成 `if`，因为：

```text
可能有多个等待者被唤醒
被唤醒不代表当前进程一定拿到了锁
必须重新检查条件
```

## 11. releasesleep：释放睡眠锁

```c
void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}
```

流程：

```text
1. 获取内部 spinlock
2. 清除 locked 和 pid
3. wakeup(lk) 唤醒等待该 channel 的进程
4. 释放内部 spinlock
```

等待者在 `acquiresleep()` 中使用同一个 `lk` 作为 channel：

```c
sleep_prepare(lk);
```

所以 `wakeup(lk)` 会唤醒等待这把 sleeplock 的进程。

## 12. spinlock 与 sleeplock 对比

| 项目 | spinlock | sleeplock |
|---|---|---|
| 等待方式 | 忙等，自旋 | 进程睡眠 |
| 适合场景 | 非常短的临界区 | 可能等待较久的资源 |
| 持有期间是否可睡眠 | 不应睡眠 | 可以用于长资源保护 |
| 是否关中断 | 获取时关闭当前 CPU 中断 | 内部短暂使用 spinlock |
| 持有者记录 | CPU | 进程 pid |
| 典型对象 | 进程状态、链表、计数器 | inode、buffer 内容 |

经验规则：

```text
短、小、不能睡眠的共享状态 -> spinlock
可能等待磁盘 IO 或长期占用的对象 -> sleeplock
```

## 13. sleep_prepare / sleep / wakeup

当前仓库的睡眠接口拆成两步：

```c
sleep_prepare(void *chan);
sleep(void);
```

这和一些教材版 xv6 中的 `sleep(chan, lk)` 形式不同。

### 13.1 sleep_prepare

```c
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}
```

作用：

```text
登记当前进程正在等待哪个 channel
```

### 13.2 sleep

```c
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}
```

如果 `p->chan != 0`，说明还没有被唤醒，于是设置进程为 `SLEEPING` 并调用 `sched()` 让出 CPU。

如果在 `sleep_prepare()` 和 `sleep()` 之间已经发生了 `wakeup(chan)`，`wakeup()` 会把 `p->chan` 清零。此时 `sleep()` 看到 `p->chan == 0`，就不会睡下去。

### 13.3 wakeup

```c
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      p->chan = 0;

      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}
```

`wakeup(chan)` 扫描所有进程：

```text
如果 p->chan == chan:
  清除 p->chan，表示唤醒事件已经发生
  如果进程已经真正睡眠，则把状态改为 RUNNABLE
```

这个设计处理了两种情况：

```text
进程已经 SLEEPING:
  wakeup 把它改回 RUNNABLE

进程刚登记 chan，但还没执行 sleep:
  wakeup 清除 chan
  之后 sleep 发现 chan 为 0，不会睡下去
```

这就是该实现避免 lost wakeup 的关键。

## 14. lost wakeup 问题

lost wakeup 指：

```text
等待者检查条件不满足，准备睡眠
唤醒者改变条件并 wakeup
等待者还没真正进入睡眠，错过 wakeup
等待者随后睡下去，永远没人再唤醒
```

当前 xv6 的规避方式是：

```text
sleep_prepare(chan) 先登记等待 channel
wakeup(chan) 可以在进程真正睡眠前清除 p->chan
sleep() 只有在 p->chan 仍非 0 时才设置 SLEEPING
```

配合 `p->lock`，`p->chan` 和 `p->state` 的变化不会被并发破坏。

## 15. 进程锁 `p->lock`

每个进程都有自己的 spinlock：

```c
struct proc {
  struct spinlock lock;
  enum procstate state;
  void *chan;
  int killed;
  int xstate;
  int pid;
  ...
};
```

`p->lock` 保护：

```text
p->state
p->chan
p->killed
p->xstate
p->pid
```

调度器依赖 `p->lock` 防止多个 hart 同时运行同一个进程：

```text
hart0 scheduler 扫描 proc[]
hart1 scheduler 也扫描 proc[]
两者都必须先 acquire(&p->lock)
只有一个 hart 能把 RUNNABLE 改为 RUNNING
```

调度路径：

```text
scheduler()
  -> acquire(&p->lock)
  -> if p->state == RUNNABLE
       p->state = RUNNING
       swtch()
  -> release(&p->lock)
```

这保证进程状态变化是互斥的。

## 16. `wait_lock`：保护父子关系

[`kernel/proc.c`](../kernel/proc.c) 中有全局锁：

```c
struct spinlock wait_lock;
```

注释说明：

```text
helps ensure that wakeups of wait()ing parents are not lost
must be acquired before any p->lock
```

它保护跨多个进程的父子关系：

```text
p->parent
exit() 唤醒父进程
wait() 扫描子进程
reparent() 把孤儿进程交给 init
```

这是单个 `p->lock` 无法完整保护的，因为这些操作常常同时涉及父进程和多个子进程。

## 17. per-CPU 数据与 `mycpu()` / `myproc()`

xv6 有每 CPU 数据：

```c
struct cpu cpus[NCPU];
```

每个 CPU 结构中包含：

```c
struct cpu {
  struct proc *proc;
  struct context context;
  int noff;
  int intena;
};
```

作用：

```text
proc     当前 CPU 正在运行的进程
context  调度器上下文
noff     当前 CPU 关中断嵌套层数
intena   最外层 push_off 前的中断状态
```

`mycpu()` / `myproc()` 需要关中断保护，因为如果当前进程在读取过程中被调度到其他 CPU，结果可能不一致。

`myproc()` 实现中会：

```text
push_off()
读取 mycpu()->proc
pop_off()
```

这保证读取当前 CPU 状态期间不会被中断和调度打断。

## 18. 启动阶段的 hart 同步

[`kernel/main.c`](../kernel/main.c) 中：

```c
volatile static int started = 0;
```

hart 0 完成全局初始化：

```c
kinit();
kvminit();
procinit();
trapinit();
plicinit();
binit();
iinit();
fileinit();
virtio_disk_init();
userinit();
__atomic_thread_fence(__ATOMIC_SEQ_CST);
started = 1;
```

其他 hart 等待：

```c
while (started == 0)
  ;
__atomic_thread_fence(__ATOMIC_SEQ_CST);
```

作用：

```text
hart 0 初始化全局内核子系统
其他 hart 必须等初始化完成后才能开启分页、trap、PLIC 并进入 scheduler
```

`__atomic_thread_fence(__ATOMIC_SEQ_CST)` 保证其他 hart 看到 `started = 1` 时，也能看到 hart 0 在此之前完成的初始化写入。

## 19. 文件系统中的锁分层

文件系统使用多层锁，避免大锁覆盖所有路径。

典型锁：

```text
bcache.lock       保护 buffer cache 全局链表
buf.sleeplock     保护单个 buffer 内容
itable.lock       保护 inode cache
inode.sleeplock   保护单个 inode 内容
ftable.lock       保护全局 file table
log.lock          保护日志状态
```

设计思路：

```text
全局元数据和链表 -> spinlock，短时间持有
具体 inode/buffer 内容 -> sleeplock，允许等待较久
磁盘 IO 路径 -> 避免持有普通 spinlock 长时间等待
```

例如 buffer cache 通常分两层：

```text
先用 bcache.lock 找到/分配 struct buf
再用 buf 的 sleeplock 保护该块数据内容
```

这样既保护全局结构，又避免在磁盘 IO 或长时间数据访问期间持有全局自旋锁。

## 20. 典型锁使用场景

spinlock 常见场景：

```text
kmem.lock       空闲物理页链表
pid_lock        nextpid 分配
wait_lock       父子进程关系
p->lock         单个进程状态
tickslock       ticks
ftable.lock     全局文件表
log.lock        文件系统日志状态
bcache.lock     buffer cache 元数据
```

sleeplock 常见场景：

```text
inode 内容
buffer 数据块内容
```

sleep/wakeup 常见场景：

```text
pipe 读写等待
console 输入等待
磁盘 IO 完成等待
wait() 等待子进程退出
pause()/uptime 相关 tick 等待
sleeplock 等待
```

## 21. xv6 没有实现的高级同步

xv6 为了教学简化，没有实现很多生产内核常见机制：

```text
RCU
读写锁
信号量抽象
futex
NUMA 感知
per-CPU slab
优先级继承
复杂死锁检测
```

这不是缺陷，而是教学取舍。xv6 用很少的代码展示 SMP 同步最核心的问题：

```text
互斥
内存顺序
中断重入
睡眠等待
丢失唤醒
多 CPU 调度竞态
```

## 22. 总结

xv6 的锁实现可以压缩为：

```text
spinlock:
  原子交换抢锁
  acquire/release 内存顺序
  当前 CPU 关中断
  保护短临界区

sleeplock:
  内部用 spinlock 保护状态
  等待时 sleep，不忙等
  释放时 wakeup
  保护可能长时间占用的资源

sleep/wakeup:
  用 channel 表示等待条件
  用 p->lock 保护 chan/state
  用 sleep_prepare + sleep 避免 lost wakeup

SMP 同步:
  per-CPU 数据降低共享
  per-proc lock 防止多 hart 同时运行同一进程
  启动 fence 保证 hart 初始化顺序
```

最核心的一句话：

```text
xv6 的 SMP 同步不是靠某一把锁完成，而是靠 spinlock、关中断、原子内存顺序、sleeplock、sleep/wakeup 和 per-object locking 共同完成。
```
