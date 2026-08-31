# xv6 中断处理简明指南

## 1. 总体模型

xv6 的中断模型非常直接：CPU 被硬件事件打断后进入 trap，内核识别中断源，调用对应设备处理函数，完成后返回原执行流。

```text
用户态被中断：

User Process
    │
    │ interrupt / exception / syscall
    ▼
uservec
    │
    ▼
usertrap()
    │
    ├── devintr()
    │      ├── timer
    │      ├── UART
    │      └── VirtIO
    │
    └── usertrapret()
           │
           ▼
          sret
```

内核态被中断：

```text
Kernel execution
    │
    │ interrupt
    ▼
kernelvec
    │
    ▼
kerneltrap()
    │
    ▼
devintr()
    │
    ▼
return to interrupted kernel code
```

---

## 2. 用户态 trap 与内核态 trap

xv6-riscv 分成两条主要 trap 路径：

- `uservec -> usertrap()`：CPU 原先运行在用户态；
- `kernelvec -> kerneltrap()`：CPU 原先已经运行在内核态。

用户态进入 trap 时，需要保存用户寄存器并切换到该进程的 kernel stack；内核态被中断时，本来就已经处于内核执行环境，因此直接保存必要寄存器并继续使用当前内核栈。

---

## 3. `trapframe` 与 `context`

xv6 中要区分两套“现场”。

### `struct trapframe`

保存用户态现场，例如：

```text
user pc
user sp
a0-a7
s0-s11
...
```

它回答的问题是：

> 这个进程最终从内核返回用户态时，从哪里继续？

### `struct context`

用于进程和 scheduler 之间的内核上下文切换，保存：

```text
ra
sp
s0-s11
```

它回答的问题是：

> 这个进程当前在内核中暂停在哪里，未来如何从 `swtch()` 之后继续？

因此在时钟抢占中：

```text
用户现场       -> trapframe
内核暂停现场   -> context
```

---

## 4. xv6 中断上下文的特点

xv6 没有像 Linux 那样建立明确、丰富的：

```text
process context
hardirq context
softirq context
```

语义体系。

普通 IRQ handler 通常直接借用当前 CPU 的执行环境：

- 如果中断时正在运行进程 P 的内核代码，就运行在 P 的 kernel stack 上；
- 如果 CPU 正在 scheduler，则运行在 scheduler stack 上；
- IRQ handler 本身不是一个独立可调度线程。

因此更准确地说：

> xv6 的中断代码是异步插入当前 CPU 执行流的一段内核代码，而不是独立的 IRQ task。

---

## 5. 外部设备中断与 PLIC

对于外部设备 IRQ，xv6 通过 PLIC 识别中断源。

典型流程：

```text
Device
   │
   ▼
PLIC
   │
   ▼
devintr()
   │
   ├── plic_claim()
   │
   ├── UART IRQ
   │      └── uartintr()
   │
   ├── VirtIO IRQ
   │      └── virtio_disk_intr()
   │
   └── plic_complete()
```

核心思想是：

```text
claim IRQ
    ↓
identify source
    ↓
run device handler
    ↓
complete IRQ
```

---

## 6. 时钟中断与抢占

时钟中断处理并不是在 `clockintr()` 内直接切进 scheduler。

更接近：

```text
timer interrupt
     ↓
usertrap()/kerneltrap()
     ↓
devintr()
     ↓
clockintr()
     ↓
更新 ticks / wakeup
     ↓
devintr() return
     ↓
trap 外层判断 timer
     ↓
yield()
     ↓
sched()
     ↓
swtch()
```

`yield()` 会：

```text
P.state = RUNNABLE
      ↓
sched()
      ↓
swtch(&p->context, &cpu->context)
```

此时 P 的 kernel stack 完整保留。未来重新调度 P 时：

```text
scheduler
   ↓
swtch()
   ↓
sched() return
   ↓
yield() return
   ↓
trap return
```

因此：

> 中断处理并没有“不返回”，而是返回路径可能被 scheduler 暂停，之后再继续。

---

## 7. 中断 handler 与 `sleep/wakeup`

设备 IRQ handler 的典型职责是：

```text
读取设备状态
更新完成状态
wakeup() 等待者
ack/complete IRQ
尽快返回
```

而不是在 handler 内等待。

经典 I/O 模型：

```text
Process P                   Device IRQ
    │                           │
submit I/O                     │
    │                           │
sleep(chan, lock)              │
    │                           │
    └──────── blocked           │
                                ▼
                         interrupt handler
                                │
                         mark completion
                                │
                         wakeup(chan)
                                │
                                ▼
                              return

P later becomes RUNNABLE
    ↓
scheduler
    ↓
sleep() returns
```

因此可记：

> 等待 I/O 的进程 `sleep()`，完成 I/O 的中断 handler `wakeup()`。

---

## 8. 中断期间为什么通常不能随意 `sleep()`

xv6 的 `sleep()` 本质是让当前 `proc` 进入：

```text
SLEEPING
   ↓
sched()
```

但设备 IRQ handler 不是独立的线程，它只是借用当前 CPU 的执行环境。

因此在真正设备 handler 中随意睡眠会破坏：

- 锁状态；
- IRQ 完成流程；
- 当前被中断执行流；
- 中断嵌套和调度假设。

所以 xv6 的典型设计也是：

```text
device IRQ handler
    ↓
不 sleep
    ↓
wakeup() others
    ↓
return
```

---

## 9. RISC-V 硬件配合

RISC-V trap 进入时，硬件会保存 trap 相关状态，并控制中断使能位。

Supervisor 模式关键寄存器包括：

```text
sepc
scause
sstatus
stvec
```

进入 trap 时，Supervisor interrupt enable 状态被保存，并临时关闭当前 S-mode 可屏蔽中断；`sret` 再恢复相关状态。

因此中断模型是软硬件共同完成的：

```text
CPU trap mechanism
      +
xv6 trap code
      +
PLIC/device state
```

---

## 10. xv6 中断模型的核心总结

```text
                 Hardware
                    │
                    ▼
                   IRQ
                    │
       ┌────────────┴────────────┐
       ▼                         ▼
   uservec                    kernelvec
       │                         │
   usertrap()                 kerneltrap()
       │                         │
       └────────────┬────────────┘
                    ▼
                 devintr()
                    │
       ┌────────────┼─────────────┐
       ▼            ▼             ▼
     timer         UART         VirtIO
       │            │             │
       └────────────┴─────────────┘
                    │
                  return
                    │
             timer interrupt?
                    │
                   yes
                    ▼
                  yield()
                    │
                  sched()
                    │
                  swtch()
```

最值得记住的几句话：

1. xv6 IRQ handler 不是独立线程，而是插入当前 CPU 执行流。
2. `trapframe` 保存用户态返回现场，`context` 保存内核调度暂停现场。
3. 设备 handler 通常只做必要硬件处理和 `wakeup()`，不应随意睡眠。
4. timer IRQ 完成设备级处理后，trap 外层可通过 `yield()` 触发抢占。
5. xv6 的模型非常适合理解 Linux hardirq 的底层基础，但缺少 Linux 的 Generic IRQ、softirq、threaded IRQ、workqueue 等复杂执行层次。
