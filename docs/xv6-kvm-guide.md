# xv6-riscv 虚拟地址、页表与内核映射指南

本文总结 xv6-riscv 中虚拟地址映射、Sv39 分页、页表和页表项、TLB、内核 direct map、同一物理页多次映射、`TRAMPOLINE`/`TRAPFRAME`，以及每进程内核栈的组织方式。

相关源码：

- [`kernel/memlayout.h`](../kernel/memlayout.h)：物理地址布局、`KERNBASE`、`PHYSTOP`、`TRAMPOLINE`、`TRAPFRAME`、`KSTACK`。
- [`kernel/riscv.h`](../kernel/riscv.h)：Sv39、PTE、页大小、地址索引宏、`MAXVA`。
- [`kernel/vm.c`](../kernel/vm.c)：内核页表创建、页表遍历、映射建立、TLB flush。
- [`kernel/kalloc.c`](../kernel/kalloc.c)：物理页分配器。
- [`kernel/proc.c`](../kernel/proc.c)：每进程内核栈映射、用户页表固定映射。
- [`kernel/trampoline.S`](../kernel/trampoline.S)：trap 时切换用户/内核页表。
- [`kernel/exec.c`](../kernel/exec.c)：用户程序 ELF 加载和用户地址空间建立。

## 1. 整体视角

xv6-riscv 的虚拟内存可以按这条链路理解：

```text
虚拟地址 VA
  -> Sv39 三层页表
  -> PTE 得到物理页 PA 和权限
  -> satp 指向当前根页表
  -> TLB 缓存 VA->PA 翻译结果
```

运行时有两类主要页表：

```text
kernel_pagetable:
  全局内核页表，所有 hart 在内核态共用

process pagetable:
  每个进程一张用户页表
```

当前 `satp` 决定 CPU 使用哪张页表：

```text
内核运行时:
  satp = kernel_pagetable

用户运行时:
  satp = 当前进程 pagetable
```

## 2. Sv39 分页组织

xv6 使用 RISC-V Sv39。

在 [`kernel/riscv.h`](../kernel/riscv.h)：

```c
#define SATP_SV39 (8L << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))
```

Sv39 特点：

```text
虚拟地址有效位：39 位
页大小：4KB
页表层数：3 层
每级页表：512 个 PTE
每个 PTE：8 字节
```

为什么每级页表正好一页：

```text
512 PTE * 8 bytes = 4096 bytes = 1 page
```

虚拟地址拆分：

```text
VA[38:30]  VPN[2]，level-2 页表索引
VA[29:21]  VPN[1]，level-1 页表索引
VA[20:12]  VPN[0]，level-0 页表索引
VA[11:0]   page offset
```

xv6 中对应宏：

```c
#define PXMASK         0x1FF
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va)  ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)
```

## 3. MAXVA 为什么少用一位

xv6 定义：

```c
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))
```

也就是：

```text
MAXVA = 1 << 38
```

这比 Sv39 理论上的 39 位少一位。原因是 Sv39 要求虚拟地址高位做 sign extension。xv6 为了简化，只使用低半部分虚拟地址，避免处理最高有效位为 1 的 canonical address。

所以 xv6 通常要求：

```text
0 <= va < MAXVA
```

`TRAMPOLINE` 就放在这个低半地址空间的最高页：

```c
#define TRAMPOLINE (MAXVA - PGSIZE)
```

## 4. PTE 页表项

PTE 类型：

```c
typedef uint64 pte_t;
typedef uint64 *pagetable_t; // 512 PTEs
```

常见权限位：

```c
#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access
```

PTE 主要包含：

```text
PPN，也就是物理页号
flags，也就是权限和状态位
```

xv6 常用转换宏：

```c
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)
```

含义：

```text
PA2PTE(pa)      把物理地址转成 PTE 中的 PPN 字段
PTE2PA(pte)     从 PTE 取出物理页地址
PTE_FLAGS(pte)  取出 PTE 低位 flags
```

权限的核心语义：

```text
PTE_V  映射有效
PTE_R  可读
PTE_W  可写
PTE_X  可执行
PTE_U  U-mode 可访问
```

没有 `PTE_U` 的页，即使出现在用户页表里，用户态也不能访问。

## 5. 页表遍历 `walk()`

核心函数在 [`kernel/vm.c`](../kernel/vm.c)：

```c
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
```

作用：

```text
给定根页表 pagetable 和虚拟地址 va
沿 Sv39 三层页表向下查找
返回 level-0 PTE 的地址
```

逻辑：

```text
level = 2:
  用 VPN[2] 找 PTE
  如果 PTE_V 且不是叶子，进入下一级页表
  如果不存在且 alloc=1，分配一页页表页

level = 1:
  用 VPN[1] 找 PTE
  同上

level = 0:
  返回 VPN[0] 对应 PTE
```

代码关键点：

```c
for (int level = 2; level > 0; level--) {
  pte_t *pte = &pagetable[PX(level, va)];
  if (*pte & PTE_V) {
    pagetable = (pagetable_t)PTE2PA(*pte);
  } else {
    if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
  }
}
return &pagetable[PX(0, va)];
```

这里中间级 PTE 只带 `PTE_V`，不带 `PTE_R/W/X`，表示它指向下一层页表，不是叶子映射。

## 6. 建立映射 `mappages()`

核心函数：

```c
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
```

作用：

```text
把虚拟地址范围 [va, va + size)
映射到物理地址范围 [pa, pa + size)
并设置权限 perm
```

核心逻辑：

```text
for each page:
  pte = walk(pagetable, va, 1)
  if pte 已经 PTE_V，panic remap
  *pte = PA2PTE(pa) | perm | PTE_V
```

要求：

```text
va 必须页对齐
size 必须页对齐
不能重复映射同一个 VA
```

注意：`mappages()` 禁止的是同一个虚拟页被重复映射，不禁止不同虚拟页映射到同一个物理页。

## 7. 物理内存布局

QEMU `virt` 机器的关键物理地址在 [`kernel/memlayout.h`](../kernel/memlayout.h)：

```text
0x00001000  QEMU boot ROM
0x02000000  CLINT
0x0c000000  PLIC
0x10000000  UART0
0x10001000  virtio disk
0x80000000  kernel load address / RAM start
0x88000000  PHYSTOP
```

xv6 使用：

```c
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)
```

物理页分配器初始化在 [`kernel/kalloc.c`](../kernel/kalloc.c)：

```c
freerange(end, (void *)PHYSTOP);
```

也就是说动态可分配物理页来自：

```text
PGROUNDUP(end) ... PHYSTOP
```

这些页后续可能被分配给：

```text
用户进程数据页
用户页表页
进程 trapframe
每进程内核栈
pipe buffer
其他内核使用的页
```

## 8. 内核页表 `kernel_pagetable`

内核页表由 [`kernel/vm.c`](../kernel/vm.c) 的 `kvmmake()` 创建：

```c
pagetable_t
kvmmake(void)
```

主要映射：

```text
UART0      -> UART0       PTE_R | PTE_W
VIRTIO0    -> VIRTIO0     PTE_R | PTE_W
PLIC       -> PLIC        PTE_R | PTE_W
KERNBASE   -> KERNBASE    kernel text, PTE_R | PTE_X
etext      -> etext       kernel data + RAM, PTE_R | PTE_W
TRAMPOLINE -> trampoline  PTE_R | PTE_X
KSTACK(i)  -> per-proc kernel stack PA, PTE_R | PTE_W
```

其中最重要的是 direct map：

```c
// map kernel text executable and read-only.
kvmmap(kpgtbl, KERNBASE, KERNBASE,
       (uint64)etext - KERNBASE, PTE_R | PTE_X);

// map kernel data and the physical RAM we'll make use of.
kvmmap(kpgtbl, (uint64)etext, (uint64)etext,
       PHYSTOP - (uint64)etext, PTE_R | PTE_W);
```

含义：

```text
VA = PA
```

内核可以直接用物理地址形式访问 RAM 和 MMIO。

## 9. 为什么内核映射 `etext..PHYSTOP`

这段映射：

```c
kvmmap(kpgtbl, (uint64)etext, (uint64)etext,
       PHYSTOP - (uint64)etext, PTE_R | PTE_W);
```

覆盖：

```text
kernel rodata/data/bss
end 之后的空闲物理页
将来分配给用户进程的物理页
页表页
trapframe 页
kernel stack 物理页
pipe buffer 页
```

但真正进入 `kalloc()` 空闲链表的是：

```text
end ... PHYSTOP
```

不是 `etext..end`。因此要区分：

```text
内核页表映射范围：etext..PHYSTOP
动态物理页分配范围：end..PHYSTOP
```

内核这样映射的好处是简单。例如 `kalloc()` 返回一页物理地址后，内核可以直接：

```c
memset(mem, 0, PGSIZE);
```

因为这页物理内存在内核页表中已经有：

```text
VA = PA
```

同样，`copyin()` / `copyout()` 通过 `walkaddr()` 得到用户页的物理地址后，内核也能直接把这个 PA 当成可访问地址使用。

## 10. 用户页表

每个进程有自己的用户页表：

```c
struct proc {
  pagetable_t pagetable;
  uint64 sz;
  struct trapframe *trapframe;
  ...
};
```

用户页表由 [`kernel/proc.c`](../kernel/proc.c) 的 `proc_pagetable()` 创建。

固定映射：

```text
TRAMPOLINE -> trampoline physical page   PTE_R | PTE_X
TRAPFRAME  -> p->trapframe physical page PTE_R | PTE_W
```

代码：

```c
mappages(pagetable, TRAMPOLINE, PGSIZE,
         (uint64)trampoline, PTE_R | PTE_X);

mappages(pagetable, TRAPFRAME, PGSIZE,
         (uint64)(p->trapframe), PTE_R | PTE_W);
```

注意：这两个映射都没有 `PTE_U`。

含义：

```text
它们在用户页表里存在
但 U-mode 不能访问
S-mode 在使用用户页表时可以访问
```

普通用户内存由 `exec()` 和 `sbrk()` 建立：

```text
text/data/bss/heap/stack
```

这些页通常带 `PTE_U`，用户态可以访问。

## 11. 用户页表不包含内核 direct map

用户进程运行时：

```text
satp = process pagetable
```

这个页表通常不包含：

```text
KERNBASE -> PHYSTOP
UART0/VIRTIO0/PLIC
kernel data
kernel stacks
```

所以用户态访问内核地址，例如：

```text
0x80000000
```

会 page fault。这就是 `usertests kernmem` 能验证用户不能读取内核内存的原因。

隔离由两层共同保证：

```text
当前 satp 指向用户页表
用户可访问页必须带 PTE_U
```

## 12. 同一物理页能否被多个页表映射

可以。

页表只是地址翻译表：

```text
VA page -> PA page + permissions
```

同一物理页可以同时出现在内核页表和用户页表中。例如某个用户数据页：

```text
物理页 PA = 0x80234000

kernel_pagetable:
  VA 0x80234000 -> PA 0x80234000
  权限 PTE_R | PTE_W

process pagetable:
  VA 0x00004000 -> PA 0x80234000
  权限 PTE_R | PTE_W | PTE_U
```

这不是复制物理页，而是两个虚拟地址指向同一个物理页。

图示：

```text
同一物理页 PA 0x80234000

kernel VA 0x80234000 ─────┐
                          ├──> PA 0x80234000
user VA   0x00004000 ─────┘
```

内核 direct map 是内核访问该物理页的通道；用户页表映射是用户进程访问该物理页的通道。物理页所有权仍由内核分配器和进程页表管理决定。

## 13. 同一张页表里多个 PTE 能否指向同一 PA

也可以。

同一张页表里多个虚拟页可以映射到同一物理页，这叫：

```text
alias mapping
synonym mapping
```

硬件翻译时只关心当前 VA 对应的 PTE，不会扫描整张页表检查是否有别的 PTE 也指向相同 PA。

xv6 中典型例子是 `TRAMPOLINE`。

内核页表中，一方面内核 text 被 direct map：

```text
VA = trampoline 的普通内核地址
  -> PA = trampoline physical page
```

另一方面，xv6 又把 trampoline 物理页映射到最高虚拟地址：

```text
VA = TRAMPOLINE
  -> PA = trampoline physical page
```

所以同一张 `kernel_pagetable` 中可能有：

```text
direct-map VA      -> trampoline PA
TRAMPOLINE high VA -> trampoline PA
```

这种映射是允许的，但要谨慎：

```text
权限可能不一致
写一个 VA 读另一个 VA 会看到同一物理内容
解除映射时不能重复 kfree 同一物理页
TLB/cache alias 问题在复杂系统中需要认真处理
```

xv6 对这种 alias 使用很克制，主要用于 trampoline 和 kernel stack 这类明确场景。

## 14. TRAMPOLINE 的作用

`TRAMPOLINE` 定义：

```c
#define TRAMPOLINE (MAXVA - PGSIZE)
```

它映射 [`kernel/trampoline.S`](../kernel/trampoline.S) 中的一页代码：

```text
uservec  用户态 trap 进入内核入口
userret  内核返回用户态出口
```

为什么需要它：

```text
用户态发生 trap 时，硬件切到 S-mode
但硬件不会自动切换页表
此时 satp 仍然是用户页表
CPU 会跳到 stvec 指向的地址
```

因此 `stvec` 必须指向一段在用户页表下也能取指的代码。xv6 把 `TRAMPOLINE` 同时映射到用户页表和内核页表的同一虚拟地址：

```text
用户页表:
  TRAMPOLINE -> trampoline PA

内核页表:
  TRAMPOLINE -> trampoline PA
```

并且两个页表里的虚拟地址必须相同。因为 `uservec` 执行过程中会切换 `satp`：

```text
用户页表 -> 内核页表
```

切换前后当前 PC 仍在执行 trampoline 代码。如果两个页表中地址不同，切换页表后当前 PC 就可能失效。

进入内核时，`uservec` 做：

```text
保存用户寄存器到 TRAPFRAME
切换到进程内核栈
切换到 kernel_pagetable
跳到 usertrap()
```

返回用户时，`userret` 做：

```text
切换到用户页表
恢复用户寄存器
sret 回到 U-mode
```

## 15. TRAPFRAME 的作用

`TRAPFRAME` 定义：

```c
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
```

每个进程有一页 trapframe：

```c
struct trapframe *trapframe;
```

用户页表中：

```text
TRAPFRAME -> p->trapframe physical page
```

但没有 `PTE_U`，用户态不能访问。

trapframe 保存：

```text
用户通用寄存器
用户 epc
kernel_satp
kernel_sp
kernel_trap
kernel_hartid
```

`trampoline.S` 使用固定虚拟地址 `TRAPFRAME` 保存/恢复寄存器。因为每个进程的用户页表都把自己的 trapframe 物理页映射到同一个虚拟地址，所以 trampoline 代码可以统一使用：

```text
TRAPFRAME
```

而不需要知道当前进程的实际物理地址。

## 16. 每进程内核栈为什么单独映射

内核页表 direct map 已经覆盖了 `etext..PHYSTOP`，所以 `kalloc()` 分配出的内核栈物理页已经可以通过：

```text
VA = PA
```

访问。

但 xv6 不直接用 direct-map 地址作为进程内核栈，而是单独映射到高地址：

```c
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)
```

映射代码在 [`kernel/proc.c`](../kernel/proc.c)：

```c
void
proc_mapstacks(pagetable_t kpgtbl)
{
  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}
```

布局：

```text
TRAMPOLINE
guard page
kernel stack for proc 0
guard page
kernel stack for proc 1
guard page
kernel stack for proc 2
...
```

单独映射的目的：

```text
给每个进程固定的内核栈虚拟地址 KSTACK(i)
在栈旁边保留 unmapped guard page
捕获内核栈溢出
避免栈溢出悄悄覆盖其他内核数据
```

因此同一个 kernel stack 物理页通常也有两个内核映射：

```text
direct map:
  VA = PA -> PA

kernel stack mapping:
  VA = KSTACK(i) -> PA
```

xv6 实际作为栈使用的是 `KSTACK(i)` 这个高地址映射。

## 17. 为什么每个进程需要内核栈

一个进程有两类栈：

```text
用户栈:
  U-mode 运行用户代码时使用
  位于用户地址空间
  用户程序可修改

内核栈:
  该进程进入 S-mode 内核后使用
  位于内核地址空间
  用户态不可访问
```

不能用用户栈作为内核栈，原因：

```text
用户栈不可信，用户可以随意修改 sp 和栈内容
用户栈可能未映射或越界
内核返回地址、局部变量不能放在用户可写内存
否则用户可能篡改内核调用栈或泄露内核数据
```

为什么是每个进程一个内核栈：

```text
进程可能在内核中 sleep
sleep 时内核调用链还保存在该进程内核栈上
CPU 会切去运行其他进程
如果多个进程共用一个内核栈，调用链会被覆盖
```

例子：

```text
进程 A:
  read()
    -> fileread()
    -> readi()
    -> virtio_disk_rw()
    -> sleep()
    -> sched()

CPU 切换到进程 B

进程 A 的内核调用链必须仍然保留在自己的内核栈上
```

每 CPU 一个栈也不够，因为进程可以在内核中阻塞并离开当前 CPU。内核栈需要跟随进程，而不是只跟随 CPU。

xv6 trap 返回用户态前设置下一次 trap 要使用的内核栈：

```c
p->trapframe->kernel_sp = p->kstack + PGSIZE;
```

用户态 trap 进入 `trampoline.S:uservec` 后：

```asm
ld sp, 8(a0)
```

从此内核 C 代码就在该进程的内核栈上运行。

## 18. TLB 与 `sfence.vma`

TLB 是 CPU 内部的地址翻译缓存：

```text
VA -> PA + permissions
```

页表在内存中，三级遍历成本较高。CPU 会把最近用过的翻译结果缓存到 TLB。

访问路径：

```text
CPU 访问 VA
  -> 查 TLB
  -> 命中：直接得到 PA 和权限
  -> 未命中：硬件 page table walk
  -> 读取 PTE 得到 PA/权限
  -> 填入 TLB
```

如果 OS 修改了页表或切换了 `satp`，TLB 中可能有旧翻译。因此需要：

```asm
sfence.vma
```

xv6 中启用内核页表：

```c
void
kvminithart()
{
  sfence_vma();
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}
```

trampoline 切换页表时也执行 `sfence.vma`：

```asm
csrw satp, t1
sfence.vma zero, zero
```

返回用户页表时：

```asm
csrw satp, a0
sfence.vma zero, zero
```

## 19. 页表切换路径

从用户进入内核：

```text
用户程序运行
  satp = process pagetable
  trap 发生
  硬件跳到 TRAMPOLINE:uservec
  仍在用户页表
  保存寄存器到 TRAPFRAME
  加载 kernel_satp
  csrw satp, kernel_satp
  sfence.vma
  跳到 usertrap()
```

从内核返回用户：

```text
usertrap()
  -> prepare_return()
  -> 计算 user satp
  -> 跳到 TRAMPOLINE:userret
  -> csrw satp, user satp
  -> sfence.vma
  -> 恢复用户寄存器
  -> sret
```

`TRAMPOLINE` 的双重映射保证切换页表前后代码地址都有效。

## 20. 权限隔离

xv6 依靠页表权限位和当前特权级实现隔离：

```text
用户普通内存:
  PTE_U = 1

TRAMPOLINE/TRAPFRAME:
  出现在用户页表
  但 PTE_U = 0

内核 direct map:
  只在 kernel_pagetable 中
  不在用户普通页表中
```

因此：

```text
U-mode 不能访问内核 direct-map 地址
U-mode 不能访问 TRAPFRAME
U-mode 不能直接执行 TRAMPOLINE
S-mode 可以在用户页表下访问无 PTE_U 的 TRAMPOLINE/TRAPFRAME
```

## 21. 常见问题总结

### 21.1 内核映射了所有剩余 RAM，用户页是否也在其中

是。

内核页表 direct-map 了 `etext..PHYSTOP`，后续分配给用户进程的物理页也在这个范围内。

但这不代表用户能通过内核地址访问它们。用户运行时使用用户页表，用户页表不包含内核 direct map。

### 21.2 同一物理页能否被内核页表和用户页表同时记录

可以。

这是正常设计。物理页只有一份，页表映射可以有多份。内核 direct map 是内核访问通道，用户页表映射是用户访问通道。

### 21.3 同一内核页表里的多个 PTE 能否指向同一物理页

可以。

典型例子是 `TRAMPOLINE` 和每进程内核栈：

```text
TRAMPOLINE:
  direct-map VA -> trampoline PA
  high VA       -> trampoline PA

kernel stack:
  direct-map VA -> stack PA
  KSTACK(i) VA  -> stack PA
```

### 21.4 为什么内核栈不直接用 direct-map 地址

因为 xv6 想要：

```text
固定 KSTACK(i) 虚拟地址
每个栈旁边有 guard page
栈溢出时尽早 page fault
```

direct map 没有这种 guard page 布局。

### 21.5 为什么进程需要自己的内核栈

因为进程可能在内核中 sleep，内核调用链必须随进程保存。用户栈不可信，每 CPU 栈也不足以保存阻塞进程的内核现场。

## 22. 总结

xv6-riscv 的虚拟内存组织可以概括为：

```text
Sv39:
  三层页表，4KB 页，satp 指向根页表

kernel_pagetable:
  direct map 设备和 RAM
  text R/X，data/RAM R/W
  单独映射 TRAMPOLINE
  单独映射 KSTACK(i) 并保留 guard page

process pagetable:
  映射用户 text/data/bss/heap/stack
  固定映射 TRAMPOLINE/TRAPFRAME
  TRAMPOLINE/TRAPFRAME 不带 PTE_U

物理页:
  可以被多个页表映射
  也可以在同一页表中被多个 VA alias 到
  所有权由 kalloc/uvmunmap/uvmfree 管理，不由映射次数自动决定

TLB:
  缓存 VA->PA
  切换 satp 或修改页表后用 sfence.vma 清理

内核栈:
  每进程一页
  映射到 KSTACK(i)
  旁边有 guard page
  用户不可访问
```

核心理解：

```text
物理内存只有一份；
虚拟映射可以有多份；
当前 satp 决定地址如何解释；
PTE_U 和 S/U mode 决定用户能否访问；
TRAMPOLINE 解决 trap 时页表切换的代码连续性；
每进程内核栈解决内核执行现场的安全保存问题。
```
