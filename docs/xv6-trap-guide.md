# xv6 Trap、上下文切换、exec 与时钟中断指南

本文以当前仓库 `xv6-riscv-enhance` 的实现为准，系统梳理用户态与内核态之间的 trap 路径，并重点解释以下容易混淆的问题：

- 用户进程陷入内核后，用户上下文、内核栈和调度上下文分别放在哪里；
- `kexec()` 明明在内核中执行了 `return`，为什么成功的 `exec()` 不会返回旧用户程序；
- 释放旧用户页表时，为什么不能释放 `TRAMPOLINE` 和 `TRAPFRAME` 对应的物理页；
- 普通系统调用返回用户态后，原来的内核执行上下文去了哪里；
- 时钟中断发生在用户态和内核态时，处理路径有什么不同；
- 多 hart 环境中，哪个 hart 处理时钟中断；
- 为什么 trapframe 要记录 `kernel_hartid`；
- 为什么 `kerneltrap()` 必须在调用 `yield()` 前保存 `sepc` 和 `sstatus`。

相关源码：

- [`kernel/trampoline.S`](../kernel/trampoline.S)：用户态 trap 入口 `uservec` 和返回入口 `userret`。
- [`kernel/kernelvec.S`](../kernel/kernelvec.S)：内核态 trap 入口 `kernelvec`。
- [`kernel/trap.c`](../kernel/trap.c)：`usertrap()`、`kerneltrap()`、`prepare_return()`、`devintr()` 和 `clockintr()`。
- [`kernel/proc.c`](../kernel/proc.c)：进程、调度、内核栈、`yield()`、`sched()` 和页表生命周期。
- [`kernel/proc.h`](../kernel/proc.h)：`trapframe`、`context`、`cpu` 和 `proc` 的定义。
- [`kernel/exec.c`](../kernel/exec.c)：`kexec()` 加载并提交新用户地址空间。
- [`kernel/syscall.c`](../kernel/syscall.c)：系统调用分发和返回值写回。
- [`kernel/sysfile.c`](../kernel/sysfile.c)：`sys_exec()`。
- [`kernel/vm.c`](../kernel/vm.c)：`uvmunmap()`、`uvmfree()` 和页表释放。
- [`kernel/start.c`](../kernel/start.c)：每个 hart 的 timer 初始化。

## 1. 首先建立正确的心智模型

在 xv6 中，一个用户进程发生 trap 后，并不是“变成了另一个内核进程”。更准确的说法是：

> 同一个 `struct proc` 的执行流，从 U-mode 切换到 S-mode，并开始使用内核页表和该进程的内核栈。

xv6 中每个 `proc` 只有一条执行线程。它在两种状态之间交替：

```text
用户态执行
  用户页表
  用户栈
  用户寄存器
        |
        | trap：系统调用、异常或中断
        v
内核态执行
  内核页表
  p->kstack
  usertrap/syscall/内核函数调用链
        |
        | userret + sret
        v
用户态继续执行
```

从用户态进入内核和从进程切换到调度器，是两种完全不同的切换，不能混为一谈。

## 2. 三类上下文和一个硬件现场

xv6 中最重要的三个软件状态容器如下。

| 状态 | 保存位置 | 保存什么 | 何时使用 |
|---|---|---|---|
| 用户上下文 | `p->trapframe` | 用户 GPR、用户 `epc/sp`，以及下一次进入内核所需信息 | U-mode 与 S-mode 之间切换 |
| 内核调度上下文 | `p->context` | `ra`、`sp`、`s0-s11` | `swtch()` 在进程与 scheduler 之间切换 |
| 内核函数调用链 | `p->kstack` | `usertrap()`、系统调用和内核函数的栈帧 | 进程在内核中运行、睡眠或被抢占 |

此外，`sepc`、`sstatus`、`scause`、`stvec` 等 CSR 属于当前 hart，而不是某个进程。调度切换不会自动为每个进程保存所有 hart CSR。

### 2.1 `trapframe` 不是调度上下文

`trapframe` 保存用户寄存器，例如：

```c
struct trapframe {
  uint64 kernel_satp;
  uint64 kernel_sp;
  uint64 kernel_trap;
  uint64 epc;
  uint64 kernel_hartid;
  uint64 ra;
  uint64 sp;
  // ...其余用户通用寄存器...
};
```

它解决的是：

```text
如何暂停用户代码，进入内核，再恢复用户代码
```

### 2.2 `context` 不是完整 trapframe

`p->context` 只包含调用约定要求跨函数调用保留的内核寄存器：

```c
struct context {
  uint64 ra;
  uint64 sp;
  uint64 s0;
  // ...
  uint64 s11;
};
```

它解决的是：

```text
如何暂停一条尚未结束的内核调用链，运行 scheduler，之后再接着执行
```

其他 caller-saved 寄存器要么已经由调用者处理，要么保存在当前内核栈上的 trap frame 中。

## 3. 用户态 trap 的完整路径

用户态发生系统调用、异常或中断时，硬件先完成最低限度的特权级切换：

- 将被中断的用户 PC 写入 `sepc`；
- 将原因写入 `scause`；
- 在 `sstatus.SPP` 中记录 trap 来自 U-mode；
- 关闭 S-mode 中断；
- 跳转到当前 hart 的 `stvec`。

硬件不会自动切换页表，也不会自动切换内核栈。剩余工作由 [`kernel/trampoline.S`](../kernel/trampoline.S) 中的 `uservec` 完成。

### 3.1 `uservec` 保存用户现场

刚进入 `uservec` 时：

```text
特权级：S-mode
页表：  仍是用户页表
sp：    仍是用户栈指针
```

`uservec` 利用固定虚拟地址 `TRAPFRAME` 找到 `p->trapframe`，保存用户通用寄存器，然后加载：

```asm
ld sp, 8(a0)    # trapframe->kernel_sp
ld tp, 32(a0)   # trapframe->kernel_hartid
ld t0, 16(a0)   # trapframe->kernel_trap，即 usertrap
ld t1, 0(a0)    # trapframe->kernel_satp
```

随后切换到内核页表并跳转到 `usertrap()`：

```asm
csrw satp, t1
sfence.vma zero, zero
jalr t0
```

### 3.2 `usertrap()` 区分系统调用、设备中断和异常

`usertrap()` 首先保存用户 PC：

```c
p->trapframe->epc = r_sepc();
```

对于系统调用，`sepc` 指向 `ecall` 本身。普通系统调用应当返回到 `ecall` 后面的指令，所以：

```c
p->trapframe->epc += 4;
syscall();
```

对于时钟中断和页面错误，不会执行这个 `+4`。它们最终应重新执行或继续执行被中断 PC 对应的指令流。

### 3.3 `prepare_return()` 准备下一次进入内核

返回用户态前，`prepare_return()` 填写：

```c
p->trapframe->kernel_satp = r_satp();
p->trapframe->kernel_sp = p->kstack + PGSIZE;
p->trapframe->kernel_trap = (uint64)usertrap;
p->trapframe->kernel_hartid = r_tp();
```

这四个字段不是当前内核调用栈的快照，而是下一次 `uservec` 的最小启动环境：

| 字段 | 下一次 `uservec` 如何使用 |
|---|---|
| `kernel_satp` | 切换到内核页表 |
| `kernel_sp` | 使用该进程的内核栈顶 |
| `kernel_trap` | 跳到 `usertrap()` |
| `kernel_hartid` | 恢复内核约定的 `tp = hartid` |

它还设置 `sstatus.SPP=0`、`sstatus.SPIE=1`，并执行：

```c
w_sepc(p->trapframe->epc);
```

### 3.4 `userret` 切回用户页表并执行 `sret`

`usertrap()` 返回的 `a0` 是新用户页表的 `satp`。`userret`：

1. 切换到用户页表；
2. 从 `TRAPFRAME` 恢复用户通用寄存器；
3. 恢复用户 `sp` 和用户 `tp`；
4. 执行 `sret`。

`sret` 使用 `sepc` 和 `sstatus`：

```text
PC          <- sepc
目标特权级   <- sstatus.SPP
中断状态     <- sstatus.SPIE
```

## 4. `kexec()` 返回了，为什么成功的 `exec()` 不返回旧程序

“成功的 exec 不返回”描述的是用户态控制流，而不是说内核 C 函数不能执行 `return`。

用户系统调用桩等价于：

```asm
exec:
    li a7, SYS_exec
    ecall
    ret
```

普通系统调用进入 `usertrap()` 后，默认返回地址被设为：

```c
p->trapframe->epc = ecall_pc + 4;
```

因此普通系统调用会回到桩中的 `ret`。

### 4.1 正常调用链

正常用户态 `exec()` 的内核调用链是：

```text
uservec
  -> usertrap
    -> syscall
      -> sys_exec
        -> kexec
```

`kerneltrap()` 不是 `exec` 的正常分发入口。只有当 `kexec()` 执行期间发生内核态中断时，才可能暂时进入 `kerneltrap()`。

本仓库在创建第一个 `/init` 时，还会从 `forkret()` 直接调用 `kexec()`；那一次不是用户系统调用。

### 4.2 `kexec()` 成功提交新用户镜像

`kexec()` 先在临时新页表中完成：

- ELF 校验和程序段加载；
- 用户栈分配；
- 参数字符串和 `argv[]` 布置；
- `a1` 指向新用户栈中的 `argv`。

全部成功后才提交：

```c
oldpagetable = p->pagetable;
p->pagetable = pagetable;
p->sz = sz;
p->trapframe->epc = elf.entry;
p->trapframe->sp = sp;
proc_freepagetable(oldpagetable, oldsz);

return argc;
```

最关键的是：

```c
p->trapframe->epc = elf.entry;
```

它覆盖了先前保存的 `ecall_pc + 4`。

### 4.3 `return argc` 返回给内核调用者

`kexec()` 的 `return argc` 仍然沿内核 C 调用栈逐层返回：

```text
kexec -> sys_exec -> syscall -> usertrap
```

系统调用分发器执行：

```c
p->trapframe->a0 = syscalls[num]();
```

因此新程序获得：

```text
a0 = argc
a1 = argv
sp = 新用户栈
epc = ELF 入口
```

最后 `prepare_return()` 将 `elf.entry` 写入 `sepc`，`userret` 切换到新页表，`sret` 直接进入新程序的 `start(argc, argv)`。旧系统调用桩中的 `ret` 永远不会执行。

完整路径是：

```text
旧程序 exec()
  -> ecall
  -> usertrap
  -> syscall
  -> sys_exec
  -> kexec
       替换 p->pagetable
       epc = elf.entry
       sp  = 新栈
       return argc
  -> syscall 把 argc 写入 trapframe->a0
  -> prepare_return 把 elf.entry 写入 sepc
  -> userret 切到新页表
  -> sret
  -> 新程序 start(argc, argv)
```

如果 `kexec()` 在提交前失败，旧页表和旧 `epc` 保持不变，系统调用才会真正返回旧程序并得到 `-1`。

## 5. 为什么不能释放 `TRAMPOLINE` 和 `TRAPFRAME` 的物理页

`proc_freepagetable()` 执行：

```c
uvmunmap(pagetable, TRAMPOLINE, 1, 0);
uvmunmap(pagetable, TRAPFRAME, 1, 0);
uvmfree(pagetable, sz);
```

`uvmunmap()` 的最后一个参数是 `do_free`：

```c
if (do_free) {
  uint64 pa = PTE2PA(*pte);
  kfree((void *)pa);
}
*pte = 0;
```

因此传入 `0` 表示只删除当前页表中的 PTE，不释放物理页。

### 5.1 物理页所有权

| 映射 | 物理页所有者 | exec 时处理方式 |
|---|---|---|
| `TRAMPOLINE` | 内核镜像，全系统共享 | 只解除旧页表映射 |
| `TRAPFRAME` | 当前 `struct proc` | 只解除旧页表映射 |
| 旧代码、数据、堆、用户栈 | 旧用户地址空间 | 解除映射并 `kfree()` |

`TRAMPOLINE` 指向内核 ELF 中的 trampoline 代码页，不是 `kalloc()` 返回的普通用户页，并且所有进程页表都映射同一个物理页。释放它会破坏整个系统。

`TRAPFRAME` 在 `allocproc()` 中只分配一次：

```c
p->trapframe = (struct trapframe *)kalloc();
```

执行 `kexec()` 时，新旧页表都会暂时映射同一个 `p->trapframe`：

```text
旧页表 --TRAPFRAME--+
                    +--> 同一个 p->trapframe 物理页
新页表 --TRAPFRAME--+
```

如果释放旧页表时对 `TRAPFRAME` 使用 `do_free=1`，新页表就会留下悬空映射。trapframe 真正的 `kfree()` 发生在整个进程被 `freeproc()` 销毁时。

### 5.2 为什么仍要显式解除这两个映射

`uvmfree(pagetable, sz)` 只释放 `[0, sz)` 中的普通用户页，然后调用 `freewalk()` 释放各级页表页。`TRAMPOLINE` 和 `TRAPFRAME` 位于用户虚拟地址空间顶部，不在 `[0, sz)` 中。

所以必须先清除这两个特殊叶子 PTE，但不能释放它们指向的共享或外部拥有的物理页。

## 6. 返回用户态后，内核执行上下文去了哪里

### 6.1 没有发生调度时

普通系统调用的内核调用链位于 `p->kstack`：

```text
usertrap
  -> syscall
    -> sys_xxx
      -> 更深层内核函数
```

系统调用完成时，函数逐层正常返回。每个函数恢复自己的栈帧，最终 `usertrap()` 返回时，内核 `sp` 逻辑上回到：

```c
p->kstack + PGSIZE
```

此时不需要保存一个“下次继续执行旧 `usertrap()`”的上下文，因为旧调用链已经结束。

内核栈物理内存仍然存在，旧字节也没有清零，但这些栈帧已经失效。下一次从用户态 trap 时，`uservec` 再次把 `sp` 设置到内核栈顶，建立一条新的调用链。

### 6.2 在内核中睡眠或被抢占时

如果系统调用尚未完成便执行：

```text
sleep/yield -> sched -> swtch
```

那么：

- 尚未返回的内核函数栈帧保留在 `p->kstack`；
- `sp`、`ra` 和 `s0-s11` 保存到 `p->context`；
- CPU 切换到当前 hart 的 scheduler context；
- 进程再次被调度时，恢复 `p->context`，从 `swtch()` 后面继续；
- 只有所有内核函数最终返回，内核栈才重新变为逻辑空栈。

因此：

```text
直接返回用户态：内核调用链已经结束，不需要 p->context 保存它
内核中被调度走：p->context 保存恢复点，p->kstack 保存未结束调用链
```

### 6.3 已回到用户态后，下一次一定走 `uservec`

只要已经执行 `sret` 回到 U-mode，下一次系统调用、用户异常或用户态中断都会先进入 `uservec`。

如果尚在 S-mode，又发生中断，则走：

```text
kernelvec -> kerneltrap
```

它继续使用当前内核栈，不会从 `p->kstack` 顶部重新建立 `usertrap()`。

## 7. 时钟中断：用户态与内核态的不同路径

本仓库使用 RISC-V Sstc 扩展。每个 hart 在 `start()` 中启用 `SIE_STIE` 并调用：

```c
w_stimecmp(r_time() + 1000000);
```

`stimecmp` 是每个 hart 自己的 CSR。因此不是一个全局时钟中断被分配给任意 hart，而是每个 hart 独立产生、处理并重新预约自己的时钟中断。

### 7.1 进程在用户态时

假设进程 `p` 正在 hart 2 的 U-mode 运行：

```text
hart 2 本地 timer interrupt
  -> uservec
  -> 保存用户寄存器
  -> 切换到 p->kstack 和内核页表
  -> usertrap
  -> devintr
  -> clockintr
  -> which_dev == 2
  -> yield
  -> hart 2 的 scheduler
```

`yield()` 将：

```c
p->state = RUNNABLE;
sched();
```

当前 hart 可以运行另一个进程。原进程以后可能仍由当前 hart 运行，也可能被其他 hart 选中。

时钟中断不是 `ecall`，所以不会执行 `epc += 4`。进程再次获得 CPU 后，从原来的用户 PC 继续。

### 7.2 进程正在内核态时

如果进程正在执行系统调用，且内核中断处于开启状态，时钟中断路径是：

```text
当前内核函数
  -> kernelvec
  -> 在当前 p->kstack 上保存寄存器
  -> kerneltrap
  -> devintr
  -> clockintr
  -> yield
  -> scheduler
```

此时系统调用的内核调用链尚未结束，所以 `p->kstack` 和 `p->context` 必须被保留。再次调度后，进程先回到被中断的内核指令，继续完成系统调用，之后才返回用户态。

如果内核正在持有自旋锁或显式执行了 `intr_off()`，时钟中断不会立刻进入 `kerneltrap()`，而是保持 pending，等重新开启中断后再处理。这避免了持锁状态下执行 `yield()`。

### 7.3 多 hart 分工

所有 hart 都会：

- 处理自己的 supervisor timer interrupt；
- 调用 `clockintr()`；
- 重新写自己的 `stimecmp`；
- 对当前 hart 上运行的进程提供一次抢占和调度机会。

只有 hart 0 更新全局 `ticks`：

```c
if (cpuid() == 0) {
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}
```

这样可以避免 `NCPU` 个 hart 让全局时间以 `NCPU` 倍速度增长。

时钟中断不经过 PLIC。PLIC 负责 UART、VirtIO 等 supervisor external interrupt。某个外部设备中断即使被 PLIC 交给 hart 1，也只会中断 hart 1 当前的执行流，不会替 hart 0 上的进程处理 trap。

## 8. `kernel_hartid` 的作用和正确性

### 8.1 `tp` 在用户态和内核态有不同含义

在用户态，`tp` 是普通 thread pointer，用户程序可以修改它。`userret` 必须恢复用户自己的 `tp`。

在 xv6 内核态，约定：

```text
tp = 当前 hart ID
```

因为：

```c
int cpuid() {
  return r_tp();
}

struct cpu *mycpu(void) {
  return &cpus[cpuid()];
}
```

而 `myproc()`、锁的中断嵌套计数和 scheduler context 都依赖 `mycpu()`。

`mhartid` 是 M-mode CSR，S-mode 内核不能在任意位置直接读取它，所以 xv6 在启动后长期用 `tp` 保存 hart ID。

### 8.2 `uservec` 同时保护用户 `tp` 和恢复内核 `tp`

进入 `uservec` 后：

```asm
sd tp, 64(a0)   # 保存用户 tp
ld tp, 32(a0)   # 恢复 trapframe->kernel_hartid
```

因此 `kernel_hartid` 的目的不是中断路由，而是在 trap 已经发生之后，为内核恢复正确的 `tp`。

### 8.3 为什么返回时保存的 hart ID，下一次 trap 仍然正确

假设 `prepare_return()` 正在 hart 0 上执行：

```c
p->trapframe->kernel_hartid = r_tp(); // 0
```

随后：

```text
hart 0: prepare_return
  -> userret
  -> sret
  -> 用户程序在 hart 0 运行
  -> 下一次 ecall/异常/本地中断也首先发生在 hart 0
  -> hart 0 的 uservec 恢复 tp=0
```

这依赖的不变量是：

> 进程从某个 hart 返回用户态后，在下一次 trap 发生之前，不可能迁移到其他 hart。

进程迁移必须先进入内核，执行 `yield()` 或 `sleep()`，变成 `RUNNABLE`，再由另一个 hart 的 scheduler 选中。因此迁移所需的第一步，本身就是在原 hart 上发生 trap。

如果进程从 hart 0 迁移到 hart 2：

```text
hart 0 用户态
  -> trap，uservec 使用保存的 0
  -> yield
  -> hart 2 恢复该进程的内核上下文
  -> hart 2 执行 prepare_return，覆盖 kernel_hartid=2
  -> hart 2 用户态
  -> 下一次 trap 使用保存的 2
```

`kernel_hartid` 不告诉硬件“把下一次 trap 发给哪个 hart”。同步异常和系统调用天然由执行相关指令的 hart 处理；本地 timer interrupt 也由对应 hart 处理。`kernel_hartid` 只是软件对当前 hart 身份的记录。

## 9. 为什么 `kerneltrap()` 要在 `yield()` 前保存 `sepc/sstatus`

`kerneltrap()` 开头执行：

```c
uint64 sepc = r_sepc();
uint64 sstatus = r_sstatus();
uint64 scause = r_scause();
```

时钟中断可能导致：

```c
if (which_dev == 2 && myproc() != 0)
  yield();
```

之后又恢复：

```c
w_sepc(sepc);
w_sstatus(sstatus);
```

### 9.1 中断内核时，硬件 CSR 保存了什么

假设进程 `p` 正在地址 `K` 的内核指令上运行。时钟中断发生后：

```text
sepc = K
sstatus.SPP = 1
sstatus.SPIE = 中断前的 SIE
sstatus.SIE = 0
```

`kernelvec` 保存通用寄存器并调用 `kerneltrap()`。最终 `kernelvec` 会执行 `sret`，而 `sret` 必须依靠原来的 `sepc/sstatus` 回到地址 `K` 的 S-mode 执行流。

### 9.2 `yield()` 为什么可能使 CSR 失效

`yield()` 本身不一定直接改写这两个 CSR。问题在于它会执行：

```text
yield -> sched -> swtch -> scheduler
```

当前进程暂停后，同一个 hart 可能运行其他进程。那些进程的用户返回和后续 trap 会反复改写这个 hart 的 `sepc/sstatus`。

例如：

```text
p 被中断时：sepc = p 的内核地址 K
  -> p yield
  -> hart 运行 q
  -> q 返回用户态，prepare_return 写 sepc = q 的用户地址 Uq
  -> q 又发生 trap，硬件再次覆盖 sepc/sstatus
  -> p 以后恢复
```

进程 `p` 甚至可能在另一个 hart 上恢复；新 hart 当前的 CSR 更不可能属于 `p`。

`swtch()` 只保存 `ra`、`sp` 和 `s0-s11`，不会自动保存 `sepc/sstatus`。因此 `kerneltrap()` 必须将原值保存在自己的局部变量中。这些局部变量位于 `p->kstack` 或 callee-saved 寄存器中，会随 `p->context` 和内核栈一起保留下来。

### 9.3 恢复后才能正确执行 `sret`

进程 `p` 恢复后，执行：

```c
w_sepc(sepc);
w_sstatus(sstatus);
```

然后返回 `kernelvec`。`kernelvec` 恢复通用寄存器并执行：

```asm
sret
```

于是：

```text
PC          <- p 原来的内核地址 K
目标特权级   <- S-mode
中断状态     <- p 被中断前对应的状态
```

如果不恢复，可能跳到另一个进程留下的地址，或者以错误的 `SPP/SPIE` 返回。

`scause` 不需要恢复，因为处理程序已经消费了中断原因，`sret` 也不读取 `scause`。源代码注释中所说的“供 kernelvec.S 使用”，准确地说是供 `kernelvec.S` 最后的 `sret` 指令使用。

## 10. 两条时钟抢占路径对照

### 10.1 从用户态抢占

```text
U-mode 用户代码
  -> uservec
  -> trapframe 保存用户上下文
  -> usertrap
  -> clockintr
  -> yield
  -> swtch 保存 p->context
  -> scheduler
  -> 再次调度 p
  -> yield 返回
  -> prepare_return
  -> userret
  -> sret
  -> 原用户 PC
```

### 10.2 从内核态抢占

```text
S-mode 内核函数
  -> kernelvec
  -> 当前 p->kstack 保存中断现场
  -> kerneltrap 保存 sepc/sstatus
  -> clockintr
  -> yield
  -> swtch 保存 p->context
  -> scheduler
  -> 再次调度 p
  -> yield 返回
  -> kerneltrap 恢复 sepc/sstatus
  -> kernelvec 恢复通用寄存器
  -> sret
  -> 原内核 PC
  -> 继续完成系统调用
  -> 最终 userret 返回用户态
```

同一个 `yield()` 既可以暂停用户态进程，也可以暂停执行到一半的系统调用。区别在于：

- 用户态时钟中断恢复后，`usertrap()` 准备返回用户态；
- 内核态时钟中断恢复后，`kerneltrap()` 先回到原内核指令，完成尚未结束的调用链。

## 11. 关键不变量

理解 xv6 trap 机制时，可以始终抓住以下不变量：

1. **trap 由当前执行流所在的 hart 本地处理。** `kernel_hartid` 不负责路由 trap。
2. **从用户态进入内核一定先经过 `uservec`。** 从内核态再次发生 trap 则经过 `kernelvec`。
3. **正常返回用户态时，原内核 C 调用链已经退栈。** 下一次用户 trap 从 `p->kstack` 顶部建立新调用链。
4. **内核调用链被调度走时，`p->context` 保存恢复点，`p->kstack` 保存未完成栈帧。**
5. **`trapframe` 保存用户上下文和下一次进入内核的启动信息，不等同于 `p->context`。**
6. **`sepc/sstatus` 是 hart CSR，不属于进程。** 跨越 `yield()` 时若后续 `sret` 仍需要它们，必须由当前 trap handler 保存和恢复。
7. **页表映射不等于物理页所有权。** 删除 PTE 时是否 `kfree()` 取决于物理页真正的所有者。
8. **成功的 `exec` 让内核调用链正常返回，但通过改写 `epc` 和页表切断旧用户控制流。**

## 12. 一句话总结

xv6 将不同边界上的状态保存得非常清楚：

```text
trapframe：跨越用户态与内核态
context：  跨越进程与 scheduler
kstack：   保存尚未结束的内核函数调用链
sepc/sstatus：当前 hart 的硬件 trap 返回现场
```

在此基础上：

- `exec` 通过替换用户页表和 `epc`，实现“内核返回了，但旧程序没有回来”；
- timer interrupt 通过 `yield()` 提供抢占点；
- 每个 hart 独立处理中断和调度；
- `kernel_hartid` 让 `uservec` 在下一次 trap 时恢复正确的内核 `tp`；
- `kerneltrap()` 保存 `sepc/sstatus`，保证进程即使经历调度和 hart 迁移，仍能回到原来的内核指令。
