# xv6 睡眠与唤醒优化指南

本文以当前仓库 `xv6-riscv-enhance` 的两阶段睡眠接口为基础，给出一种不引入动态等待队列、不改变现有调度器扫描方式的唤醒优化方案。

方案的核心是：

> 使用固定大小的哈希等待位图，为 `chan` 建立“可能正在等待该通道的进程槽位”索引。`wakeup()` 只检查目标哈希桶中的登记进程，不再获取整个进程表中每个进程的锁。

本文重点覆盖以下问题：

- 为什么当前 `wakeup()` 的主要开销来自全进程表扫描和逐进程加锁；
- 如何在不创建独立 `wait_queue` 对象的情况下建立等待索引；
- 如何保持 `sleep_prepare()`、`sleep()` 两阶段 API 的提前唤醒语义；
- 如何统一条件锁、等待索引锁和 `p->lock` 的获取顺序；
- 如何分别实现 `wakeup_one()` 和 `wakeup_all()`；
- 如何处理“不再睡眠”、`kill()`、超时和进程槽复用；
- 为什么该方案只优化唤醒查找，而不改变 scheduler 的全进程扫描。

相关源码：

- [`kernel/proc.c`](../kernel/proc.c)：`sleep_prepare()`、`sleep()`、`wakeup()`、`kkill()`、scheduler 和进程生命周期。
- [`kernel/proc.h`](../kernel/proc.h)：`struct proc`、进程状态和接口声明。
- [`kernel/sleeplock.c`](../kernel/sleeplock.c)：睡眠锁对两阶段睡眠接口的使用。
- [`kernel/pipe.c`](../kernel/pipe.c)：管道满/空条件上的睡眠与唤醒。
- [`kernel/uart.c`](../kernel/uart.c)：先登记唤醒、再检查设备状态的典型场景。
- [`kernel/trap.c`](../kernel/trap.c)：时钟中断中的 `wakeup(&ticks)`。

## 1. 优化目标与非目标

### 1.1 优化目标

当前 `wakeup(chan)` 遍历 `proc[]`，并依次获取每个 `p->lock`：

```text
wakeup(chan)
  -> 遍历 proc[0..NPROC)
  -> acquire(p->lock)
  -> 比较 p->chan
  -> release(p->lock)
```

即使只有一个进程等待目标通道，也要检查所有进程。单次唤醒的复杂度和锁操作数量都是 `O(NPROC)`。

本方案希望达到：

```text
sleep_prepare(chan)  O(1)
wakeup_one(chan)     平均接近 O(1)
wakeup_all(chan)     O(目标哈希桶中的登记进程数)
```

主要收益不是消除所有循环，而是减少无关 `p->lock` 的获取，降低多核环境中的锁竞争和缓存行抖动。

### 1.2 非目标

本方案明确不做以下改动：

- 不为每个 `chan` 动态创建等待队列对象；
- 不在 `struct proc` 中维护等待链表节点；
- 不引入 scheduler 运行队列；
- 不修改 `swtch()`、时间片或调度策略；
- 不改变“唤醒只设置 `RUNNABLE`，由 scheduler 决定何时运行”的语义。

优化后的总体路径如下：

```text
条件不成立
  -> sleep_prepare(chan)
  -> 在 wait_table[hash(chan)] 中登记进程槽位
  -> 释放条件锁
  -> sleep()
  -> SLEEPING

事件发生
  -> wakeup_one/all(chan)
  -> 只检查目标哈希桶中的登记槽位
  -> 清除等待登记
  -> SLEEPING -> RUNNABLE

scheduler
  -> 仍扫描 proc[]
  -> 选择 RUNNABLE 进程
  -> RUNNABLE -> RUNNING
```

## 2. 为什么选择哈希等待位图

`chan` 是任意非零内核地址，可能是：

- `&ticks`；
- `&pi->nread` 或 `&pi->nwrite`；
- `struct sleeplock *`；
- `struct buf *`；
- 某个进程指针。

若为每个 `chan` 创建独立等待队列，需要解决对象初始化、对象销毁、动态内存分配和等待节点生命周期。xv6 的进程表大小固定，因此可以使用更简单的固定索引：

```text
chan
  -> hash(chan)
  -> wait_table[bucket]
  -> NPROC 位的等待位图
```

每个置位 bit 代表对应的 `proc[]` 槽位可能登记在该哈希桶中。哈希冲突不会导致错误唤醒，因为真正修改进程前仍会检查：

```c
p->chan == chan
```

位图只是候选索引，不代替 `p->chan` 的精确匹配。

## 3. 数据结构

建议在 [`kernel/proc.c`](../kernel/proc.c) 中定义固定等待表：

```c
#define NWAIT_BUCKET 32
#define WAIT_WORD_BITS 64
#define WAIT_WORDS ((NPROC + WAIT_WORD_BITS - 1) / WAIT_WORD_BITS)

struct wait_bucket {
  struct spinlock lock;
  uint64 bits[WAIT_WORDS];
  int cursor;
};

static struct wait_bucket wait_table[NWAIT_BUCKET];
```

约束：

- `NWAIT_BUCKET` 使用 2 的幂，方便通过掩码计算桶号；
- 若 `NPROC <= 64`，每个桶只需要一个 `uint64` 位图；
- `cursor` 是 `wakeup_one()` 的轮转扫描起点，用于避免总是优先唤醒低编号进程；
- 所有内存静态分配，不在睡眠、中断或唤醒路径中调用内存分配器。

在 [`kernel/proc.h`](../kernel/proc.h) 的 `struct proc` 中增加：

```c
int wait_bucket;  // -1：未登记；其他值：登记所在的哈希桶
```

现有 `p->chan` 继续表示等待的精确通道：

```text
p->wait_bucket == -1
  -> 当前没有有效等待登记

p->wait_bucket >= 0 && p->chan != 0
  -> 当前已登记一次等待
```

哈希函数可采用简单的指针混合：

```c
static int
wait_hash(void *chan)
{
  uint64 x = (uint64)chan;
  x ^= x >> 16;
  return x & (NWAIT_BUCKET - 1);
}
```

初始化阶段需要：

```c
for(int i = 0; i < NWAIT_BUCKET; i++) {
  initlock(&wait_table[i].lock, "wait_bucket");
  memset(wait_table[i].bits, 0, sizeof(wait_table[i].bits));
  wait_table[i].cursor = 0;
}
```

每个进程槽初始化或释放时都应恢复：

```c
p->chan = 0;
p->wait_bucket = -1;
```

## 4. 全局锁顺序与不变量

### 4.1 唯一锁顺序

所有睡眠和唤醒路径必须遵守：

```text
条件锁 -> wait_bucket.lock -> p->lock
```

各路径的锁行为如下：

| 路径 | 锁顺序 |
|---|---|
| `sleep_prepare()` | 调用者已持有条件锁；内部获取 bucket，再获取 `p->lock` |
| `sleep()` | 只获取当前进程的 `p->lock` |
| `wakeup_one/all()` | 获取 bucket，再获取候选进程的 `p->lock` |
| `sleep_cancel()` | 获取 bucket，再获取 `p->lock` |
| scheduler | 只获取 `p->lock` |
| kill/timeout 取消等待 | 获取 bucket，再获取 `p->lock` |

禁止在持有 `p->lock` 时直接获取 bucket 锁：

```text
CPU 0：持有 bucket.lock，等待 p->lock
CPU 1：持有 p->lock，等待 bucket.lock
```

这种 ABBA 顺序会导致死锁。

### 4.2 等待登记不变量

建议在调试阶段检查以下不变量：

```text
p->wait_bucket == -1
  => p 对应的等待位不应出现在任何 bucket 中

p->wait_bucket == b
  => wait_table[b] 中 p 对应的 bit 已置位
  => p->chan != 0

p->state == SLEEPING
  => 正常条件睡眠时必须存在有效等待登记

进程进入 UNUSED 或被复用前
  => p->wait_bucket == -1
  => p->chan == 0
```

## 5. 两阶段睡眠 API

当前仓库把传统的 `sleep(chan, lk)` 拆成：

```c
sleep_prepare(chan);
release(lk);
sleep();
acquire(lk);
```

其核心语义是：

```text
sleep_prepare()：登记“下一次该 chan 的唤醒与我有关”
sleep()：如果登记仍然有效，则进入 SLEEPING
wakeup()：清除登记；如果已经 SLEEPING，再设置 RUNNABLE
```

这种设计必须保留，因为它可以处理发生在 `sleep_prepare()` 与 `sleep()` 之间的提前唤醒。

### 5.1 `sleep_prepare()`

伪代码如下：

```c
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();
  int bi;
  int pi;

  if(chan == 0)
    panic("sleep_prepare: zero chan");

  bi = wait_hash(chan);
  pi = p - proc;

  acquire(&wait_table[bi].lock);
  acquire(&p->lock);

  if(p->wait_bucket != -1)
    panic("sleep_prepare: already prepared");

  p->chan = chan;
  p->wait_bucket = bi;
  wait_table[bi].bits[pi / 64] |= 1ULL << (pi % 64);

  release(&p->lock);
  release(&wait_table[bi].lock);
}
```

正常条件等待仍采用：

```c
acquire(&condition_lock);

while(!condition) {
  sleep_prepare(chan);
  release(&condition_lock);
  sleep();
  acquire(&condition_lock);
}

release(&condition_lock);
```

必须先在持有条件锁时完成登记，再释放条件锁。否则仍可能出现“条件已经改变并完成唤醒，但进程尚未登记”的丢失唤醒。

### 5.2 `sleep()`

`sleep()` 不访问等待桶，只检查登记是否仍然有效：

```c
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  if(p->wait_bucket != -1) {
    p->state = SLEEPING;
    sched();
  }

  release(&p->lock);
}
```

调用 `sched()` 时只持有 `p->lock`，满足 scheduler 的现有锁约束。

### 5.3 提前唤醒为什么不会丢失

提前唤醒时序：

```text
进程 A                         进程 B / 中断
--------------------------------------------------
sleep_prepare(chan)
  设置 bit
  p->chan = chan

release(condition_lock)

                               wakeup(chan)
                                 清除 bit
                                 p->chan = 0
                                 p->wait_bucket = -1

sleep()
  发现 wait_bucket == -1
  不进入 SLEEPING
```

真正睡眠后的唤醒时序：

```text
进程 A                         进程 B / 中断
--------------------------------------------------
sleep_prepare(chan)
release(condition_lock)
sleep()
  acquire(p->lock)
  state = SLEEPING
  sched()

                               wakeup(chan)
                                 获取 bucket.lock
                                 等待并获取 p->lock
                                 清除等待登记
                                 state = RUNNABLE
```

因此等待位图不会改变两阶段 API 的 lost-wakeup 保护逻辑，它只替代“去哪里寻找候选进程”。

## 6. `wakeup_one()` 与 `wakeup_all()`

### 6.1 公共实现

建议使用一个公共内部函数：

```c
static int
wakeup_chan(void *chan, int wake_one)
{
  int bi = wait_hash(chan);
  struct wait_bucket *b = &wait_table[bi];
  int count = 0;
  int start;

  acquire(&b->lock);
  start = b->cursor;

  for(int n = 0; n < NPROC; n++) {
    int i = (start + n) % NPROC;
    uint64 mask = 1ULL << (i % 64);

    if((b->bits[i / 64] & mask) == 0)
      continue;

    struct proc *p = &proc[i];
    acquire(&p->lock);

    // 位图只能定位候选者；chan 比较负责消除哈希冲突。
    if(p->wait_bucket == bi && p->chan == chan) {
      b->bits[i / 64] &= ~mask;
      p->chan = 0;
      p->wait_bucket = -1;

      if(p->state == SLEEPING)
        p->state = RUNNABLE;

      count++;
      b->cursor = (i + 1) % NPROC;
    }

    release(&p->lock);

    if(wake_one && count != 0)
      break;
  }

  release(&b->lock);
  return count;
}
```

对外接口：

```c
int
wakeup_one(void *chan)
{
  return wakeup_chan(chan, 1);
}

int
wakeup_all(void *chan)
{
  return wakeup_chan(chan, 0);
}

void
wakeup(void *chan)
{
  wakeup_all(chan);
}
```

保留 `wakeup()` 作为 `wakeup_all()` 的兼容包装，可以先不批量修改现有调用点。

### 6.2 如何选择 one 或 all

建议原则：

| 场景 | 建议接口 | 原因 |
|---|---|---|
| 释放一把睡眠锁 | `wakeup_one()` | 一次通常只有一个进程能取得锁 |
| 信号量增加一个资源 | `wakeup_one()` | 只新增一个可消费资源 |
| 管道关闭 | `wakeup_all()` | 所有等待者都应重新检查 EOF/错误条件 |
| 日志提交完成或全局状态改变 | `wakeup_all()` | 多个等待者的条件可能同时成立 |
| 时钟 tick | `wakeup_all()` | 多个定时等待者都需要检查超时 |
| 无法证明只需唤醒一个 | `wakeup_all()` | 保持当前广播语义最安全 |

无论使用 one 还是 all，等待方都必须用 `while` 重新检查条件：

```c
while(!condition) {
  sleep_prepare(chan);
  release(&lock);
  sleep();
  acquire(&lock);
}
```

“被唤醒”只表示条件可能发生变化，不保证当前进程一定能消费资源。

### 6.3 `wakeup_one()` 的公平性

若每次从 `proc[0]` 开始查找，低编号进程更容易被选中。每个哈希桶使用 `cursor` 保存下一次扫描起点：

```c
b->cursor = (i + 1) % NPROC;
```

这提供近似轮询公平。它不是严格 FIFO，但不需要链表，也不会长期固定偏向低 PID。

## 7. 取消等待登记

### 7.1 为什么必须有 `sleep_cancel()`

两阶段 API 允许先登记，再检查硬件状态。例如 UART：

```c
sleep_prepare(&tx_chan);

if(ReadReg(LSR) & LSR_TX_IDLE) {
  WriteReg(THR, buf[i]);
} else {
  sleep();
}
```

如果设备已经空闲，当前代码不会调用 `sleep()`。在只有 `p->chan` 时，后续登记可能覆盖旧值；加入等待位图后，旧 bit 会残留在原哈希桶中。因此必须明确配对：

```text
每次 sleep_prepare()
  -> 要么调用 sleep()
  -> 要么调用 sleep_cancel()
```

UART 路径应改成：

```c
sleep_prepare(&tx_chan);

if(ReadReg(LSR) & LSR_TX_IDLE) {
  sleep_cancel();
  WriteReg(THR, buf[i]);
  i++;
} else {
  sleep();
}
```

### 7.2 取消时如何避免锁反转

为了确定 bucket，需要读取 `p->wait_bucket`；但不能持有 `p->lock` 再获取 bucket 锁。应采用“读取、释放、按正确顺序重锁、重新验证”的循环：

```c
void
sleep_cancel(void)
{
  struct proc *p = myproc();

  for(;;) {
    int bi;

    acquire(&p->lock);
    bi = p->wait_bucket;
    release(&p->lock);

    if(bi < 0)
      return;

    struct wait_bucket *b = &wait_table[bi];
    acquire(&b->lock);
    acquire(&p->lock);

    if(p->wait_bucket != bi) {
      release(&p->lock);
      release(&b->lock);
      continue;
    }

    int i = p - proc;
    b->bits[i / 64] &= ~(1ULL << (i % 64));
    p->chan = 0;
    p->wait_bucket = -1;

    release(&p->lock);
    release(&b->lock);
    return;
  }
}
```

重新验证是必要的，因为在第一次读取 bucket 编号后，另一个 CPU 的 `wakeup()` 可能已经取消了登记。

## 8. kill、超时与进程生命周期

### 8.1 kill 必须走同一套取消路径

不能只把睡眠进程改为 `RUNNABLE` 而不清除等待索引。完整逻辑是：

```text
设置 killed
  -> 定位并锁住等待 bucket
  -> 获取 p->lock
  -> 清除等待 bit
  -> p->chan = 0
  -> p->wait_bucket = -1
  -> 若 state == SLEEPING，则 state = RUNNABLE
```

对任意目标进程执行取消时，同样需要“读取 bucket、释放 `p->lock`、按 bucket -> `p->lock` 重锁并重新验证”，不能从 `p->lock` 反向获取 bucket 锁。

还必须处理 kill 发生在“登记之前”的情况。对于可被 kill 中断的等待，`sleep_prepare()` 或紧邻 `sleep()` 的路径应在 `p->lock` 保护下重新检查 `p->killed`，确保进程不会在已经收到 kill 后重新登记并睡眠。

### 8.2 超时和其他取消原因

超时、设备错误或条件已经由其他方式满足时，都必须使用同一个内部取消原语。不要分别直接修改：

```c
p->chan
p->wait_bucket
wait_table[].bits
```

建议实现一个内部函数，根据取消原因完成一致状态转换：

```c
wait_abort(struct proc *p, int make_runnable);
```

`sleep_cancel()`、kill 和超时逻辑都复用它，减少遗漏位图清理的可能。

### 8.3 进程槽复用

位图索引使用的是 `p - proc`，因此进程槽被复用前必须清除旧登记。否则旧通道的唤醒可能命中新进程。

在 `freeproc()` 或等价释放路径中建议加入断言：

```c
if(p->wait_bucket != -1 || p->chan != 0)
  panic("freeproc: process still registered in wait table");
```

若释放路径允许处理异常状态，则应先调用内部取消原语，再把进程状态改为 `UNUSED`。

## 9. scheduler 保持全进程扫描

该方案只优化：

```text
事件发生后，如何找到等待该事件的进程
```

它不优化：

```text
CPU 空闲后，如何找到 RUNNABLE 进程
```

唤醒路径仍只执行：

```c
p->state = RUNNABLE;
```

scheduler 继续扫描 `proc[]`：

```c
for(struct proc *p = proc; p < &proc[NPROC]; p++) {
  acquire(&p->lock);

  if(p->state == RUNNABLE) {
    p->state = RUNNING;
    swtch(...);
  }

  release(&p->lock);
}
```

这样做的优点是：

- 修改范围只限于睡眠和唤醒子系统；
- 不引入全局或 per-CPU 运行队列锁；
- 不处理负载均衡、重复入队、CPU affinity 等新问题；
- 可以单独验证等待索引优化是否正确。

等待索引与调度器的关系是：

```text
哈希等待位图
  -> wakeup 清除等待登记
  -> p->state = RUNNABLE
  -> scheduler 扫描 proc[] 时发现该进程
  -> p->state = RUNNING
```

## 10. 复杂度与资源开销

| 操作 | 当前实现 | 哈希等待位图 |
|---|---:|---:|
| `sleep_prepare()` | O(1) | O(1) |
| `wakeup_one()` | O(NPROC) | 平均接近 O(1)，最坏受哈希冲突影响 |
| `wakeup_all()` | O(NPROC) 且获取所有 `p->lock` | 只获取目标桶中已登记进程的锁 |
| scheduler | O(NPROC) | O(NPROC)，保持不变 |
| 动态内存分配 | 无 | 无 |
| 每进程额外数据 | 无 | 一个 `wait_bucket` 字段 |

若 `NPROC == 64`、`NWAIT_BUCKET == 32`，纯位图占用：

```text
32 buckets * 8 bytes = 256 bytes
```

另外还有每桶的 spinlock 和 cursor，整体仍是固定且很小的内核内存开销。

最坏情况下，很多不同 `chan` 哈希到同一个桶，`wakeup()` 仍可能检查较多候选进程，但不会发生错误唤醒。可通过调整 `NWAIT_BUCKET` 或改进哈希函数降低冲突率。

## 11. 推荐实施顺序

为了缩小每一步的验证范围，建议按以下顺序修改：

1. 在 `struct proc` 中增加 `wait_bucket`，并在初始化/释放路径设置为 `-1`。
2. 增加 `wait_table[]`、哈希函数、位操作辅助函数和初始化逻辑。
3. 修改 `sleep_prepare()`，使其同时设置 `p->chan` 和等待位。
4. 修改 `sleep()`，以 `wait_bucket != -1` 判断登记是否仍有效。
5. 实现 `wakeup_all()`，让兼容接口 `wakeup()` 调用它。
6. 增加 `sleep_cancel()`，修复 UART 等登记后不调用 `sleep()` 的路径。
7. 审计 `kkill()`、超时、错误退出和 `freeproc()`，统一使用取消原语。
8. 实现 `wakeup_one()`，先只在语义明确的睡眠锁路径使用。
9. 完成并发压力测试后，再评估是否扩大 `wakeup_one()` 的使用范围。

不要在同一次修改中同时引入等待索引和 scheduler 运行队列，否则出现丢失唤醒或重复调度时很难定位责任边界。

## 12. 验证清单

### 12.1 功能测试

- 管道为空时读进程睡眠，写入后能够被唤醒；
- 管道已满时写进程睡眠，读取后能够被唤醒；
- 关闭管道能够唤醒全部读写等待者；
- 多个进程竞争睡眠锁时，`wakeup_one()` 每次至少推进一个进程；
- 时钟 tick 能唤醒所有等待相应时间条件的进程；
- UART 在“设备已经空闲”和“需要等待中断”两条路径中都不会留下脏登记；
- 磁盘完成中断能够唤醒等待对应 `struct buf` 的进程；
- 日志提交完成能够唤醒所有需要重新检查日志状态的进程。

### 12.2 关键竞态测试

必须覆盖以下时序：

```text
sleep_prepare -> wakeup -> sleep
sleep_prepare -> sleep -> wakeup
sleep_prepare -> sleep_cancel
sleep_prepare -> kill -> sleep
sleep_prepare -> sleep -> kill
wakeup_one 与 wakeup_all 并发
两个不同 chan 哈希到同一个 bucket
进程退出并复用 proc 槽位
```

期望结果：

- 不出现永久睡眠；
- 不出现同一进程重复登记；
- 不出现残留等待 bit；
- 不把无关哈希冲突进程改为 `RUNNABLE`；
- 不出现 bucket 与 `p->lock` 的锁顺序死锁；
- `freeproc()` 时等待登记已经清理。

### 12.3 调试辅助

开发阶段可加入一致性检查函数：

```c
wait_check_proc(struct proc *p);
wait_check_all(void);
```

检查内容包括：

- `p->wait_bucket` 与对应 bit 是否一致；
- 未登记进程是否错误地出现在其他 bucket；
- 同一进程是否出现在多个 bucket；
- `SLEEPING` 进程是否具有有效 `chan`；
- `UNUSED` 进程是否不存在任何等待登记。

## 13. 总结

该方案使用“固定哈希桶 + 进程位图”替代 `wakeup()` 的全进程表盲扫：

```text
不创建动态或每通道等待队列
  + 保留 sleep_prepare()/sleep() 两阶段语义
  + bucket -> p->lock 的统一锁顺序
  + 支持 wakeup_one() 与 wakeup_all()
  + scheduler 继续扫描 proc[]
```

它保留 xv6 简单、静态分配和易于验证的特点，同时显著减少唤醒路径获取无关 `p->lock` 的次数。实现时最需要关注的不是哈希或位操作，而是提前唤醒、取消登记、kill、进程槽复用以及全局锁顺序的一致性。
