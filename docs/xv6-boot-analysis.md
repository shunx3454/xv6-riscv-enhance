# xv6-riscv 启动流程分析

本文专门分析本项目从 QEMU 启动到进入 xv6 shell 的完整链路，重点引用启动相关文件、函数，并解释关键代码设计。

## 1. QEMU 如何把控制权交给 xv6

项目通过 `make qemu` 启动，最终执行类似命令：

```bash
qemu-system-riscv64 \
  -machine virt \
  -bios none \
  -kernel kernel/kernel \
  -m 128M \
  -smp 3 \
  -nographic \
  -global virtio-mmio.force-legacy=false \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
```

关键参数：

- `-machine virt`：使用 QEMU 的 RISC-V `virt` 虚拟机器。
- `-bios none`：不加载 OpenSBI/BIOS，直接运行 xv6 内核。
- `-kernel kernel/kernel`：加载 xv6 kernel ELF。
- `-m 128M`：给虚拟机 128MB 内存，对应 [`kernel/memlayout.h`](../kernel/memlayout.h) 中的 `PHYSTOP`。
- `-smp 3`：启动 3 个 hart，即 3 个 RISC-V 硬件线程。
- `-nographic`：串口接到当前终端，所以 xv6 shell 直接显示在终端。
- `fs.img`：作为 virtio block 设备挂载，提供 xv6 文件系统。

QEMU `virt` 机器会把内核加载到物理地址 `0x80000000`，并让每个 hart 从那里开始执行。这个地址和 xv6 的链接脚本、内存布局是配套的。

## 2. 链接脚本固定内核入口地址

关键文件：

- [`kernel/kernel.ld`](../kernel/kernel.ld)
- [`kernel/entry.S`](../kernel/entry.S)

关键内容：

```ld
ENTRY( _entry )
. = 0x80000000;
.text : {
  kernel/entry.o(_entry)
  *(.text .text.*)
  ...
}
```

函数/符号说明：

- `_entry`：内核 ELF 的入口符号。
- `0x80000000`：QEMU 加载并跳转到的地址。
- `etext`：内核 text 段结束位置，后续建立内核页表时用于区分代码段和数据段。
- `end`：内核镜像结束位置，物理页分配器从这里之后开始管理空闲页。

代码分析：

- 链接脚本把 `kernel/entry.o(_entry)` 放在 `.text` 的最前面，保证 `0x80000000` 处就是 `_entry`。
- `trampoline` 被强制页对齐，并断言大小正好不超过一页。这是因为 trap 入口/返回代码需要被映射到固定高地址 `TRAMPOLINE`。
- `end` 是内核静态镜像的结束点，后续 [`kernel/kalloc.c`](../kernel/kalloc.c) 的 `kinit()` 会把 `end..PHYSTOP` 放入空闲页链表。

## 3. 汇编入口 `_entry`：为 C 代码准备栈

关键文件：

- [`kernel/entry.S`](../kernel/entry.S)
- [`kernel/start.c`](../kernel/start.c)

入口代码：

```asm
_entry:
        la sp, stack0
        li a0, 1024*4
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        call start
```

对应 C 侧定义：

```c
__attribute__((aligned(16))) char stack0[4096 * NCPU];
```

函数/符号说明：

- `_entry`：所有 hart 最先执行的 xv6 代码。
- `stack0`：启动早期使用的栈数组，每个 hart 分配 4096 字节。
- `mhartid`：RISC-V CSR，保存当前 hart id。
- `start()`：进入 C 代码后的第一个函数。

代码分析：

每个 hart 都从 `_entry` 进入。如果所有 hart 共用一个栈，会立刻互相覆盖调用帧。因此 `_entry` 根据 `mhartid` 计算不同的栈顶：

```text
hart 0 -> stack0 + 1 * 4096
hart 1 -> stack0 + 2 * 4096
hart 2 -> stack0 + 3 * 4096
```

这里栈向低地址增长，所以每个 hart 使用自己那段 4KB 空间。完成栈设置后，才能安全调用 C 函数 `start()`。

## 4. `start()`：从 machine mode 切到 supervisor mode

关键文件：

- [`kernel/start.c`](../kernel/start.c)
- [`kernel/riscv.h`](../kernel/riscv.h)

核心函数：

- `start()`：早期机器模式初始化。
- `timerinit()`：初始化 timer interrupt。
- `r_mstatus()` / `w_mstatus()`：读写 `mstatus`。
- `w_mepc()`：设置 `mret` 返回地址。
- `w_medeleg()` / `w_mideleg()`：把异常和中断委托给 supervisor mode。
- `w_pmpaddr0()` / `w_pmpcfg0()`：配置 PMP，让 supervisor mode 能访问物理内存。
- `w_tp()`：把 hart id 保存到 `tp` 寄存器。

关键代码：

```c
x &= ~MSTATUS_MPP_MASK;
x |= MSTATUS_MPP_S;
w_mstatus(x);

w_mepc((uint64)main);
w_satp(0);

w_medeleg(0xffff);
w_mideleg(0xffff);
w_sie(r_sie() | SIE_SEIE | SIE_STIE);

w_pmpaddr0(0x3fffffffffffffull);
w_pmpcfg0(0xf);

timerinit();

int id = r_mhartid();
w_tp(id);

asm volatile("mret");
```

代码分析：

`start()` 运行在 machine mode，但 xv6 内核主体不长期运行在 machine mode。它通过修改 `mstatus.MPP` 和 `mepc`，让 `mret` 之后进入 supervisor mode 并跳到 `main()`。

几个关键动作：

- `w_mepc((uint64)main)`：设置 `mret` 后要执行的地址。
- `w_satp(0)`：早期先关闭分页，等 `main()` 中建立页表后再启用。
- `w_medeleg(0xffff)` 和 `w_mideleg(0xffff)`：把大部分异常/中断交给 supervisor mode 处理，后续进入 `trap.c` 的逻辑。
- PMP 配置允许 supervisor mode 访问物理内存，否则 S mode 可能无法访问 RAM 和设备。
- `w_tp(id)` 把 hart id 放到 `tp`，后续 `cpuid()` 直接读 `tp`。

因此 `start()` 的本质是：完成最低限度机器级设置，然后把控制权交给 supervisor-mode 内核。

## 5. `main()`：内核子系统初始化

关键文件：

- [`kernel/main.c`](../kernel/main.c)

核心函数：

- `main()`：所有 hart 都会进入。
- `cpuid()`：返回当前 hart id。
- `consoleinit()`：初始化控制台输入输出。
- `printkinit()`：初始化内核打印锁。
- `kinit()`：初始化物理页分配器。
- `kvminit()`：创建内核页表。
- `kvminithart()`：启用当前 hart 的分页。
- `procinit()`：初始化进程表。
- `trapinit()` / `trapinithart()`：初始化 trap 处理。
- `plicinit()` / `plicinithart()`：初始化外部中断控制器。
- `binit()`：初始化 buffer cache。
- `iinit()`：初始化 inode table。
- `fileinit()`：初始化全局 file table。
- `virtio_disk_init()`：初始化 virtio 磁盘。
- `userinit()`：创建第一个用户进程。
- `scheduler()`：进入调度循环。

初始化顺序：

```c
consoleinit();
printkinit();
kinit();
kvminit();
kvminithart();
procinit();
trapinit();
trapinithart();
plicinit();
plicinithart();
binit();
iinit();
fileinit();
virtio_disk_init();
userinit();
started = 1;
scheduler();
```

多 hart 逻辑：

```c
if (cpuid() == 0) {
  // 全局初始化
  started = 1;
} else {
  while (started == 0)
    ;
  // 每个 hart 自己的初始化
}
```

代码分析：

- hart 0 负责全局资源初始化，例如页分配器、进程表、buffer cache、inode table、file table、virtio 磁盘。
- 其他 hart 等待 `started`，避免在全局资源尚未初始化完成时进入调度器。
- `__atomic_thread_fence(__ATOMIC_SEQ_CST)` 用来保证其他 hart 能看到初始化完成后的内存状态。
- 每个 hart 都必须执行自己的 `kvminithart()`、`trapinithart()`、`plicinithart()`，因为这些涉及当前 CPU 的寄存器或中断上下文。

启动日志：

```text
xv6 kernel is booting
hart 2 starting
hart 1 starting
init: starting sh
$
```

其中 `xv6 kernel is booting` 来自 hart 0，`hart n starting` 来自其他 hart。

## 6. 页表启用：从物理地址执行到虚拟地址执行

关键文件：

- [`kernel/vm.c`](../kernel/vm.c)
- [`kernel/memlayout.h`](../kernel/memlayout.h)
- [`kernel/riscv.h`](../kernel/riscv.h)

核心函数：

- `kvmmake()`：创建内核页表。
- `kvmmap()`：添加内核映射。
- `kvminit()`：保存全局 `kernel_pagetable`。
- `kvminithart()`：写 `satp` 并执行 `sfence.vma`。

内核页表映射：

```text
UART0      -> UART0
VIRTIO0    -> VIRTIO0
PLIC       -> PLIC
KERNBASE   -> KERNBASE       kernel text, R/X
etext      -> etext..PHYSTOP kernel data/RAM, R/W
TRAMPOLINE -> trampoline
KSTACK(n)  -> per-process kernel stack
```

代码分析：

`kvminithart()` 的关键操作是：

```c
sfence_vma();
w_satp(MAKE_SATP(kernel_pagetable));
sfence_vma();
```

`satp` 是 RISC-V 的页表根寄存器。写入 `satp` 后，CPU 开始使用 Sv39 页表进行地址翻译。前后两次 `sfence.vma` 用来清理旧的 TLB 状态，保证页表切换生效。

xv6 内核使用 direct map：大部分内核虚拟地址等于物理地址。这样即使打开分页，原来正在执行的内核地址仍然有效，启动路径不会断裂。

## 7. `userinit()`：创建第一个可调度进程

关键文件：

- [`kernel/proc.c`](../kernel/proc.c)

核心函数：

- `userinit()`：设置第一个进程。
- `allocproc()`：分配进程结构、trapframe、用户页表。
- `proc_pagetable()`：创建用户页表并映射 `TRAMPOLINE`、`TRAPFRAME`。

关键代码：

```c
p = allocproc();
initproc = p;
p->cwd = namei("/");
p->state = RUNNABLE;
release(&p->lock);
```

代码分析：

这个版本的 xv6 没有直接把一段 `initcode` 拷到用户内存执行，而是先创建一个空的进程结构，把它标记为 `RUNNABLE`。真正执行 `/init` 的动作放在第一次调度到该进程后的 `forkret()` 中。

这样设计的原因是：文件系统初始化 `fsinit(ROOTDEV)` 可能调用会睡眠的路径，不能在 `main()` 这种非普通进程上下文中执行。因此 xv6 等到第一个进程被调度后，在进程上下文中初始化文件系统并 `exec("/init")`。

## 8. `scheduler()` 与 `forkret()`：第一次进入用户态

关键文件：

- [`kernel/proc.c`](../kernel/proc.c)
- [`kernel/trap.c`](../kernel/trap.c)
- [`kernel/trampoline.S`](../kernel/trampoline.S)
- [`kernel/exec.c`](../kernel/exec.c)

核心函数：

- `scheduler()`：选择 `RUNNABLE` 进程运行。
- `swtch()`：切换到进程内核上下文。
- `forkret()`：进程第一次被调度时执行的返回路径。
- `fsinit()`：初始化文件系统。
- `kexec()`：加载 `/init`。
- `prepare_return()`：准备返回用户态。
- `userret`：汇编代码，切到用户页表并执行 `sret`。

关键代码：

```c
if (first) {
  fsinit(ROOTDEV);
  first = 0;
  p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
  if (p->trapframe->a0 == -1) {
    panic("exec");
  }
}

prepare_return();
uint64 satp = MAKE_SATP(p->pagetable);
uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
((void (*)(uint64))trampoline_userret)(satp);
```

代码分析：

`allocproc()` 会把进程的内核上下文返回地址设置为 `forkret`。因此调度器第一次 `swtch()` 到该进程时，不是从普通系统调用返回，而是从 `forkret()` 开始执行。

`forkret()` 做三件关键事：

1. 第一次运行时初始化文件系统。
2. 执行 `kexec("/init", ...)`，把用户进程地址空间替换成 `/init` 程序。
3. 调用 `prepare_return()` 和 trampoline 的 `userret`，真正返回用户态。

`prepare_return()` 会设置：

- `stvec`：下一次用户态 trap 进入 `uservec`。
- `trapframe->kernel_satp`：下次 trap 时切回内核页表。
- `trapframe->kernel_sp`：下次 trap 时使用该进程内核栈。
- `trapframe->kernel_trap`：下次 trap 调用 `usertrap()`。
- `sepc`：用户态入口地址，即 `/init` ELF 的入口。
- `sstatus.SPP = 0`：让 `sret` 返回 user mode。

这一步完成后，CPU 从 supervisor mode 返回 user mode，开始执行 `/init`。

## 9. `/init`：启动 shell

关键文件：

- [`user/init.c`](../user/init.c)
- [`user/sh.c`](../user/sh.c)

核心函数：

- `main()`：`/init` 的用户态入口。
- `open()`、`mknod()`、`dup()`：设置标准输入输出错误。
- `fork()`：创建 shell 子进程。
- `exec("sh", argv)`：执行 shell。
- `wait()`：等待 shell 退出，并在退出后重启 shell。

关键代码：

```c
if (open("console", O_RDWR) < 0) {
  mknod("console", CONSOLE, 0);
  open("console", O_RDWR);
}
dup(0);
dup(0);

for (;;) {
  printf("init: starting sh\n");
  pid = fork();
  if (pid == 0) {
    exec("sh", argv);
    exit(1);
  }
  ...
  wpid = wait((int *)0);
}
```

代码分析：

`/init` 是第一个用户态程序。它首先确保 `console` 设备存在并打开为 fd 0，然后通过两次 `dup(0)` 设置 stdout 和 stderr：

```text
fd 0 -> console stdin
fd 1 -> console stdout
fd 2 -> console stderr
```

之后 `/init` 进入无限循环，启动 shell 并等待 shell 退出。如果 shell 退出，`init` 会重新启动一个 shell。这就是 xv6 中 `$` 提示符反复可用的原因。

## 10. 完整启动时序

完整链路可以压缩为：

```text
QEMU
  -> load kernel/kernel at 0x80000000
  -> _entry
  -> set per-hart stack
  -> start()
  -> configure machine-mode registers
  -> mret to supervisor mode
  -> main()
  -> hart 0 initializes global kernel subsystems
  -> other harts wait for started
  -> userinit()
  -> scheduler()
  -> swtch to first proc
  -> forkret()
  -> fsinit(ROOTDEV)
  -> kexec("/init")
  -> prepare_return()
  -> trampoline userret
  -> sret to user mode
  -> user/init.c main()
  -> open console
  -> fork
  -> exec("sh")
  -> shell prints "$"
```

启动成功时可以看到：

```text
xv6 kernel is booting
hart 2 starting
hart 1 starting
init: starting sh
$
```

## 11. 启动设计要点

几个最重要的设计点：

1. **链接地址和 QEMU 加载地址一致**

   `kernel.ld` 把 `_entry` 放在 `0x80000000`，QEMU 也从这里跳入内核。

2. **每个 hart 必须先有独立栈**

   `_entry` 根据 `mhartid` 分配启动栈，否则多核会破坏同一块栈内存。

3. **machine mode 只做过渡**

   `start()` 设置好特权级、异常委托、PMP、timer 后，立刻通过 `mret` 进入 supervisor mode。

4. **hart 0 做全局初始化**

   `main()` 中只有 hart 0 初始化全局子系统，其他 hart 等待 `started`，防止并发初始化。

5. **文件系统初始化放在普通进程上下文**

   `forkret()` 中调用 `fsinit(ROOTDEV)`，因为文件系统路径可能睡眠，不适合在 `main()` 中执行。

6. **第一次用户态运行通过 `exec("/init")` 完成**

   `userinit()` 只创建可调度进程，真正的用户程序由 `forkret()` 中的 `kexec("/init")` 加载。

7. **从内核返回用户态依赖 trampoline**

   `prepare_return()` 设置寄存器和 trapframe，`trampoline.S:userret` 切换到用户页表并 `sret`。

理解这些点后，xv6 的启动流程就不只是“调用一串初始化函数”，而是一个从硬件入口、特权级切换、页表启用、多核同步、进程创建，到用户态 shell 的完整闭环。
