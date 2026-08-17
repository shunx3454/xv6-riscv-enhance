# xv6-riscv 关键模块分析

本文按操作系统职责划分本项目的主要模块，列出关键文件、核心函数和需要重点理解的代码路径。源码引用均为相对仓库路径。

## 1. 启动与平台初始化

关键文件：

- [`kernel/entry.S`](../kernel/entry.S)
- [`kernel/start.c`](../kernel/start.c)
- [`kernel/main.c`](../kernel/main.c)
- [`kernel/kernel.ld`](../kernel/kernel.ld)
- [`kernel/memlayout.h`](../kernel/memlayout.h)

核心函数/入口：

- `_entry`：RISC-V 启动入口，建立每个 hart 的早期栈，然后跳到 `start()`。
- `start()`：运行在 machine mode，设置特权级、异常委托、PMP、timer，然后 `mret` 进入 supervisor mode。
- `timerinit()`：配置 supervisor timer interrupt。
- `main()`：内核主初始化函数。

启动路径：

```text
QEMU -kernel kernel/kernel
  -> 0x80000000
  -> kernel/entry.S:_entry
  -> kernel/start.c:start()
  -> kernel/main.c:main()
  -> userinit()
  -> scheduler()
  -> forkret()
  -> exec("/init")
```

代码分析：

- [`kernel/kernel.ld`](../kernel/kernel.ld) 将内核链接到 `0x80000000`，这和 QEMU `virt` 机器加载内核的位置一致。
- [`kernel/entry.S`](../kernel/entry.S) 根据 `mhartid` 为每个 CPU/hart 选择不同的启动栈，避免多核启动时栈冲突。
- [`kernel/start.c`](../kernel/start.c) 只做硬件特权级相关的最低限度初始化，随后长期运行在 supervisor mode。
- [`kernel/main.c`](../kernel/main.c) 中 hart 0 负责全局初始化，其他 hart 等待 `started` 后只做本 hart 的页表、trap、PLIC 初始化。

`main()` 的初始化顺序很重要：

```text
console/printk
  -> kalloc
  -> kernel page table
  -> proc table
  -> trap
  -> plic
  -> buffer cache / inode / file table
  -> virtio disk
  -> first user process
  -> scheduler
```

这种顺序保证后续模块依赖已经就绪。例如 `userinit()` 需要进程表和页表；文件系统真正初始化 `fsinit()` 放到 `forkret()` 中，是因为文件系统初始化可能睡眠，必须处于普通进程上下文。

## 2. 内存布局与地址空间

关键文件：

- [`kernel/memlayout.h`](../kernel/memlayout.h)
- [`kernel/riscv.h`](../kernel/riscv.h)
- [`kernel/vm.c`](../kernel/vm.c)
- [`kernel/kalloc.c`](../kernel/kalloc.c)
- [`user/user.ld`](../user/user.ld)

核心函数/宏：

- `KERNBASE`：内核和物理 RAM 起始地址，`0x80000000`。
- `PHYSTOP`：xv6 使用的物理内存结束地址，当前为 `KERNBASE + 128MB`。
- `TRAMPOLINE`：用户/内核切换代码的固定高地址映射。
- `TRAPFRAME`：每个进程保存用户寄存器的固定高地址页面。
- `kinit()`、`kalloc()`、`kfree()`：物理页分配器。
- `kvmmake()`、`kvminit()`、`kvminithart()`：内核页表建立和启用。

物理内存规划：

```text
0x00001000    QEMU boot ROM
0x02000000    CLINT
0x0c000000    PLIC
0x10000000    UART0
0x10001000    virtio disk
0x80000000    kernel load address / RAM start
0x88000000    PHYSTOP
```

内核镜像布局：

```text
0x80000000
  _entry
  .text
  trampoline
  etext
  .rodata
  .data
  .bss
end
  free physical pages
PHYSTOP
```

内核虚拟地址规划：

- 设备 MMIO：`UART0`、`VIRTIO0`、`PLIC` 直接映射。
- 内核 text：映射为 `PTE_R | PTE_X`。
- 内核 data 和可用 RAM：映射为 `PTE_R | PTE_W`。
- `TRAMPOLINE`：映射到最高虚拟地址附近，供 trap 入口/返回使用。
- 每个进程的内核栈：映射在高地址，旁边放无效 guard page。

用户虚拟地址规划：

```text
0x0
  text
  rodata
  data
  bss
  heap via sbrk/sbrklazy
  user stack
  ...
TRAPFRAME
TRAMPOLINE
MAXVA
```

代码分析：

- [`kernel/kalloc.c`](../kernel/kalloc.c) 的分配器只管理 `end` 到 `PHYSTOP` 之间的 4KB 页，不会把内核镜像或 MMIO 地址放进空闲链表。
- [`kernel/vm.c`](../kernel/vm.c) 的 `kvmmake()` 使用 direct map，内核虚拟地址大体等于物理地址，简化了内核访问物理内存和设备寄存器的逻辑。
- `TRAMPOLINE` 同时映射在用户页表和内核页表的同一虚拟地址，这是因为 trap 刚发生时 CPU 仍在用户页表下执行，必须先有一段在两个页表中都可执行的代码完成页表切换。

## 3. 进程管理与调度

关键文件：

- [`kernel/proc.h`](../kernel/proc.h)
- [`kernel/proc.c`](../kernel/proc.c)
- [`kernel/swtch.S`](../kernel/swtch.S)

核心数据结构：

- `struct proc`：进程控制块。
- `struct cpu`：每个 hart 的 CPU 状态。
- `struct context`：内核上下文切换保存的寄存器集合。
- `struct trapframe`：用户态寄存器保存区。

核心函数：

- `procinit()`：初始化进程表。
- `allocproc()`：分配并初始化一个 `struct proc`。
- `userinit()`：创建第一个用户进程。
- `kfork()`：实现 `fork()`。
- `kexit()`：实现 `exit()`。
- `kwait()`：实现 `wait()`。
- `scheduler()`：每个 CPU 的调度循环。
- `sched()`、`yield()`：进程主动让出 CPU。
- `sleep()`、`wakeup()`：阻塞与唤醒。
- `swtch()`：汇编级上下文切换。

进程状态：

```text
UNUSED
USED
SLEEPING
RUNNABLE
RUNNING
ZOMBIE
```

调度路径：

```text
scheduler()
  -> scan proc[]
  -> find RUNNABLE
  -> p->state = RUNNING
  -> swtch(&cpu->context, &p->context)
  -> process runs
  -> yield/sleep/exit
  -> swtch(&p->context, &cpu->context)
  -> scheduler resumes
```

代码分析：

- `scheduler()` 是简单 round-robin：遍历固定大小的 `proc[]`，选择第一个 `RUNNABLE` 进程运行。
- `swtch()` 只保存 callee-saved 寄存器和栈指针/返回地址，不保存所有用户寄存器。用户寄存器由 trap 路径保存到 `trapframe`。
- `sleep(chan, lk)` 的关键点是先拿 `p->lock` 再释放条件锁 `lk`，避免 `wakeup()` 和睡眠状态切换之间丢失唤醒。
- `wait_lock` 用来保护父子关系，避免 `exit()`、`wait()`、`reparent()` 在多核下产生竞态。

`fork()` 需要复制的资源：

```text
user page table + user memory
trapframe
open file references
current working directory
process name
```

`exit()` 不会立刻释放 `struct proc`，而是进入 `ZOMBIE`，等待父进程 `wait()` 回收。这和 Unix 进程模型一致。

## 4. 虚拟内存、页表与 lazy allocation

关键文件：

- [`kernel/vm.c`](../kernel/vm.c)
- [`kernel/sysproc.c`](../kernel/sysproc.c)
- [`kernel/exec.c`](../kernel/exec.c)
- [`kernel/riscv.h`](../kernel/riscv.h)

核心函数：

- `walk()`：遍历 Sv39 三层页表。
- `walkaddr()`：把用户虚拟地址翻译为物理地址。
- `mappages()`：建立页表映射。
- `uvmcreate()`：创建空用户页表。
- `uvmalloc()`、`uvmdealloc()`：增长/缩小用户地址空间。
- `uvmcopy()`：fork 时复制用户内存。
- `copyin()`、`copyout()`、`copyinstr()`：用户/内核之间安全拷贝。
- `vmfault()`：处理 lazy allocation 缺页。
- `sys_sbrk()`：调整进程内存大小。
- `kexec()`：加载 ELF 到用户页表。

Sv39 页表结构：

```text
VA[38:30]  level-2 index
VA[29:21]  level-1 index
VA[20:12]  level-0 index
VA[11:0]   page offset
```

代码分析：

- `walk()` 按 2 -> 1 -> 0 级页表向下走，需要时分配中间页表页。
- `walkaddr()` 要求 PTE 同时有效并带 `PTE_U`，因此内核映射和 trampoline 这类不带用户权限的页不会被用户访问。
- `copyin()` / `copyout()` 不直接信任用户指针，而是逐页翻译并拷贝，避免用户传入内核地址或非法地址。
- 本项目的 `sbrk()` 有 eager 和 lazy 两种路径：
  - eager：`growproc()` 立即分配物理页。
  - lazy：只增加 `p->sz`，访问时由 `usertrap()` 调用 `vmfault()` 补页。

lazy allocation 路径：

```text
user calls sbrklazy()
  -> sys_sbrk()
  -> p->sz += n
  -> user touches address
  -> page fault
  -> usertrap()
  -> vmfault()
  -> kalloc()
  -> mappages()
  -> return to user
```

如果 page fault 地址不在 `p->sz` 范围内，或已经映射，或分配失败，`vmfault()` 返回 0，`usertrap()` 会杀掉该进程。这就是 `usertests` 中很多非法访问测试能通过的原因。

## 5. Trap、中断与系统调用

关键文件：

- [`kernel/trampoline.S`](../kernel/trampoline.S)
- [`kernel/trap.c`](../kernel/trap.c)
- [`kernel/kernelvec.S`](../kernel/kernelvec.S)
- [`kernel/syscall.c`](../kernel/syscall.c)
- [`kernel/sysproc.c`](../kernel/sysproc.c)
- [`kernel/sysfile.c`](../kernel/sysfile.c)
- [`user/usys.pl`](../user/usys.pl)

核心函数：

- `uservec`：用户态 trap 的汇编入口。
- `usertrap()`：处理来自用户态的 syscall、interrupt、exception。
- `prepare_return()`：准备返回用户态。
- `userret`：切回用户页表并 `sret`。
- `kerneltrap()`：处理内核态 trap。
- `devintr()`：区分 timer、UART、virtio 等中断。
- `syscall()`：按系统调用号分发。
- `argraw()`、`argint()`、`argaddr()`、`argstr()`：系统调用参数获取。

系统调用路径：

```text
user wrapper
  -> ecall
  -> trampoline.S:uservec
  -> save registers to trapframe
  -> switch to kernel page table
  -> trap.c:usertrap()
  -> syscall.c:syscall()
  -> sysproc.c / sysfile.c
  -> prepare_return()
  -> trampoline.S:userret
  -> switch to user page table
  -> sret
```

代码分析：

- `user/usys.pl` 生成用户态 syscall stub：把系统调用号放入 `a7`，执行 `ecall`，返回值在 `a0`。
- `usertrap()` 看到 `scause == 8` 时认为是 system call，并将 `epc += 4`，避免返回后再次执行同一条 `ecall`。
- `scause == 13` 或 `15` 可能是 lazy allocation 的 load/store page fault；如果 `vmfault()` 不能处理，就认为是非法访问并杀掉进程。
- `prepare_return()` 会设置 `stvec` 到 `uservec`，设置 `sepc` 为用户 PC，并通过 `sstatus` 让下一次 `sret` 返回 user mode。

RISC-V trap 硬件不会自动保存全部通用寄存器，也不会自动切换页表。xv6 依赖 `trampoline.S` 显式保存寄存器、切换 `satp`、刷新 TLB，这是理解 RISC-V 版本 xv6 的关键。

## 6. 文件系统、文件表与日志

关键文件：

- [`kernel/fs.c`](../kernel/fs.c)
- [`kernel/fs.h`](../kernel/fs.h)
- [`kernel/file.c`](../kernel/file.c)
- [`kernel/file.h`](../kernel/file.h)
- [`kernel/sysfile.c`](../kernel/sysfile.c)
- [`kernel/bio.c`](../kernel/bio.c)
- [`kernel/log.c`](../kernel/log.c)
- [`mkfs/mkfs.c`](../mkfs/mkfs.c)

核心函数：

- `fsinit()`：读取 superblock，初始化日志。
- `balloc()`、`bfree()`：分配/释放磁盘块。
- `ialloc()`、`iget()`、`idup()`、`iput()`：inode 生命周期管理。
- `ilock()`、`iunlock()`：inode sleeplock。
- `bmap()`：文件逻辑块到磁盘块映射。
- `readi()`、`writei()`：inode 数据读写。
- `dirlookup()`、`dirlink()`：目录项查询和链接。
- `namex()`、`namei()`、`nameiparent()`：路径解析。
- `filealloc()`、`filedup()`、`fileclose()`：全局 file 表管理。
- `fileread()`、`filewrite()`：统一处理 inode、pipe、device 文件。
- `begin_op()`、`end_op()`、`log_write()`：文件系统事务日志。

文件系统层次：

```text
sysfile.c
  -> file.c
  -> fs.c
  -> log.c
  -> bio.c
  -> virtio_disk.c
```

代码分析：

- `sysfile.c` 是系统调用层，负责用户参数检查、fd 分配、事务边界。
- `file.c` 把不同文件类型统一成 `struct file`，包括普通 inode、pipe、device。
- `fs.c` 是 xv6 文件系统主体，包含块分配、inode、目录、路径解析和读写。
- `bio.c` 是 buffer cache，缓存磁盘块并提供 sleeplock，避免多个进程同时修改同一块。
- `log.c` 实现 redo log。文件系统系统调用一般以 `begin_op()` 开始，以 `end_op()` 结束；最后一个 outstanding operation 结束时提交事务。

典型 `write()` 路径：

```text
sys_write()
  -> argfd()
  -> filewrite()
  -> begin_op()
  -> writei()
  -> bmap()
  -> bread()
  -> log_write()
  -> end_op()
  -> commit if needed
```

日志设计的重点是：磁盘块修改先写入 log，提交点是写 log header，之后再 install 到 home location。这样即使崩溃，也可以通过 header 判断是否有完整事务需要恢复。

## 7. 设备驱动与中断控制

关键文件：

- [`kernel/uart.c`](../kernel/uart.c)
- [`kernel/console.c`](../kernel/console.c)
- [`kernel/virtio_disk.c`](../kernel/virtio_disk.c)
- [`kernel/plic.c`](../kernel/plic.c)
- [`kernel/virtio.h`](../kernel/virtio.h)
- [`kernel/memlayout.h`](../kernel/memlayout.h)

核心函数：

- `uartinit()`、`uartputc()`、`uartgetc()`、`uartintr()`：UART 串口驱动。
- `consoleinit()`、`consoleread()`、`consolewrite()`、`consoleintr()`：控制台字符设备。
- `virtio_disk_init()`、`virtio_disk_rw()`、`virtio_disk_intr()`：virtio block 设备。
- `plicinit()`、`plicinithart()`、`plic_claim()`、`plic_complete()`：PLIC 外部中断控制。

设备地址：

```text
UART0     0x10000000
VIRTIO0   0x10001000
PLIC      0x0c000000
```

代码分析：

- `console.c` 将 UART 封装成 xv6 的字符设备，普通用户程序通过 `read(0, ...)` 和 `write(1, ...)` 间接访问。
- `virtio_disk.c` 通过 virtqueue 和 QEMU 提供的 virtio block 设备交换请求，xv6 的 `fs.img` 就是通过它作为磁盘挂载进来。
- `plic.c` 负责外部中断分发。`devintr()` 从 PLIC claim irq，根据 irq 调用 `uartintr()` 或 `virtio_disk_intr()`，最后 complete irq。

设备中断路径：

```text
device raises interrupt
  -> PLIC
  -> kernelvec / usertrap
  -> devintr()
  -> plic_claim()
  -> uartintr() or virtio_disk_intr()
  -> plic_complete()
```

## 8. 锁、同步与并发

关键文件：

- [`kernel/spinlock.c`](../kernel/spinlock.c)
- [`kernel/spinlock.h`](../kernel/spinlock.h)
- [`kernel/sleeplock.c`](../kernel/sleeplock.c)
- [`kernel/sleeplock.h`](../kernel/sleeplock.h)
- [`kernel/proc.c`](../kernel/proc.c)

核心函数：

- `initlock()`、`acquire()`、`release()`：自旋锁。
- `push_off()`、`pop_off()`：关闭/恢复本 CPU 中断，防止持锁时被中断打断造成死锁。
- `initsleeplock()`、`acquiresleep()`、`releasesleep()`：睡眠锁。
- `sleep()`、`wakeup()`：进程睡眠与唤醒。

代码分析：

- `spinlock` 用于短临界区，例如进程表、pid 分配、buffer cache 元数据。
- `sleeplock` 允许持锁者睡眠，适合磁盘 IO 相关对象，例如 inode 和 buffer。
- xv6 默认以多 hart 方式运行，因此共享结构必须加锁。
- `sleep(chan, lk)` 的接口设计很关键：调用者持有条件锁 `lk` 进入，`sleep()` 内部在设置进程睡眠状态时原子地释放 `lk`，醒来后重新获取 `lk`。

锁的常见使用场景：

```text
proc lock       保护单个进程状态
wait_lock       保护父子关系
bcache lock     保护 buffer cache 链表
inode sleeplock 保护 inode 内容
log lock        保护日志状态
```

## 9. 用户态程序、运行库与测试

关键文件：

- [`user/init.c`](../user/init.c)
- [`user/sh.c`](../user/sh.c)
- [`user/ulib.c`](../user/ulib.c)
- [`user/umalloc.c`](../user/umalloc.c)
- [`user/printf.c`](../user/printf.c)
- [`user/usys.pl`](../user/usys.pl)
- [`user/user.h`](../user/user.h)
- [`user/usertests.c`](../user/usertests.c)

核心程序/函数：

- `init`：第一个用户进程，负责打开 `console` 并启动 `sh`。
- `sh`：xv6 shell，支持基本命令、重定向、管道。
- `start()`：用户程序入口包装，调用 `main()` 后自动 `exit()`。
- `sbrk()`、`sbrklazy()`：用户态内存增长接口。
- `malloc()`、`free()`：简单用户态堆分配器。
- `usertests`：综合回归测试。

用户程序构建：

- `Makefile` 中 `UPROGS` 列出要打进文件系统镜像的用户程序。
- `user/usys.pl` 生成 `user/usys.S`，为每个系统调用生成 `ecall` stub。
- `mkfs/mkfs.c` 把 `README` 和用户程序写入 `fs.img`。

代码分析：

- 用户程序不能直接调用内核函数，只能通过 syscall stub 执行 `ecall`。
- `init` 如果发现没有 `console` 设备，会通过 `mknod("console", CONSOLE, 0)` 创建控制台设备节点。
- `usertests` 中很多 `unexpected scause` 日志是有意触发非法访问；只要内核杀掉子进程，测试就是成功的。

## 10. 推荐阅读主线

如果目标是快速建立全局理解，建议按下面顺序阅读：

```text
1. kernel/main.c
2. kernel/proc.c
3. kernel/trap.c + kernel/trampoline.S
4. kernel/vm.c + kernel/kalloc.c
5. kernel/syscall.c + kernel/sysproc.c + kernel/sysfile.c
6. kernel/fs.c + kernel/log.c + kernel/bio.c
7. kernel/virtio_disk.c + kernel/uart.c + kernel/plic.c
8. user/init.c + user/sh.c + user/usertests.c
```

主线心智模型：

```text
boot
  -> initialize kernel subsystems
  -> create first user process
  -> schedule process
  -> user code traps into kernel for syscall/interrupt/page fault
  -> kernel manipulates process, memory, file system, device
  -> return to user
```

理解这条闭环后，再分别深入调度、页表、文件系统和设备驱动，会更容易把代码串起来。
