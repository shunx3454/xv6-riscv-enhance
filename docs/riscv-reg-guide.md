# RISC-V 64 寄存器与特权级访问指南

本文总结 RISC-V 64，尤其是 xv6-riscv 项目中涉及的寄存器分类、各特权级对寄存器的可见性/可访问性/可操作性，以及这些寄存器在 xv6 启动、trap、页表切换、时钟中断中的实际用法。

相关源码：

- [`kernel/riscv.h`](../kernel/riscv.h)：xv6 对 RISC-V CSR 和页表宏的封装。
- [`kernel/entry.S`](../kernel/entry.S)：启动入口，读取 `mhartid`，设置早期栈。
- [`kernel/start.c`](../kernel/start.c)：machine mode 初始化，切换到 supervisor mode。
- [`kernel/trap.c`](../kernel/trap.c)：trap、系统调用、中断、page fault 处理。
- [`kernel/trampoline.S`](../kernel/trampoline.S)：用户态/内核态切换时保存寄存器和切换页表。
- [`kernel/vm.c`](../kernel/vm.c)：写 `satp` 启用 Sv39 页表。

## 1. RISC-V 寄存器宏观分类

RISC-V 64 中常见寄存器可以分成几类：

```text
通用寄存器 GPR       x0-x31
程序计数器 PC        隐式存在，不能像普通寄存器一样直接读写
浮点寄存器 FPR       f0-f31，需要 F/D 扩展
向量寄存器 Vector    v0-v31，需要 V 扩展
控制状态寄存器 CSR   m* / s* / u* / h*
```

对 xv6-riscv 来说，最重要的是：

```text
GPR
PC
M-mode CSR
S-mode CSR
satp
time / stimecmp
sscratch
```

xv6 的 Makefile 使用 `-march=rv64gc`，表示目标 ISA 是：

```text
RV64I + M + A + F + D + C
```

即 64 位基础整数指令集，加整数乘除、原子指令、单精度浮点、双精度浮点、压缩指令。`RV64GC` 说明处理器有通用 64 位 RISC-V 能力，但 xv6 本身主要依赖的是 RISC-V privileged architecture，也就是特权级、CSR、trap、页表和中断机制。

## 2. RISC-V 特权级

xv6 主要涉及三个特权级：

```text
M-mode  Machine mode，最高权限，启动早期使用
S-mode  Supervisor mode，操作系统内核运行
U-mode  User mode，用户程序运行
```

访问权限大体规则：

```text
M-mode 可以访问 M/S/U 级 CSR
S-mode 可以访问 S/U 级 CSR，不能直接访问 M 级 CSR
U-mode 只能访问 U 级 CSR，不能直接访问 S/M 级 CSR
```

如果低特权级代码访问高特权级 CSR，一般会触发 illegal instruction exception。

xv6 启动路径：

```text
QEMU
  -> M-mode 进入 kernel/entry.S:_entry
  -> kernel/start.c:start()
  -> 配置 mstatus/mepc/medeleg/mideleg/PMP/timer
  -> mret
  -> S-mode kernel/main.c:main()
  -> 创建用户进程
  -> sret
  -> U-mode /init 和 sh
```

## 3. 通用寄存器 GPR

RISC-V 有 32 个通用寄存器：

```text
x0 - x31
```

常用 ABI 名称：

```text
x0   zero    永远为 0，写入无效
x1   ra      return address
x2   sp      stack pointer
x3   gp      global pointer
x4   tp      thread pointer
x5   t0      temporary
x6   t1
x7   t2
x8   s0/fp   saved register / frame pointer
x9   s1
x10  a0      argument / return value
x11  a1
x12  a2
x13  a3
x14  a4
x15  a5
x16  a6
x17  a7      xv6 系统调用号
x18  s2
...
x27  s11
x28  t3
x29  t4
x30  t5
x31  t6
```

可见性和访问性：

| 模式 | 可见 | 可读写 | 说明 |
|---|---:|---:|---|
| U-mode | 是 | 是 | 用户程序正常使用 |
| S-mode | 是 | 是 | 内核正常使用 |
| M-mode | 是 | 是 | 启动和机器级代码正常使用 |

注意点：

- GPR 不是每个模式一套，而是一组架构寄存器在不同模式下共同使用。
- 发生 trap 时，硬件不会自动保存所有 GPR。
- xv6 必须在 [`kernel/trampoline.S`](../kernel/trampoline.S) 中手动把用户寄存器保存到 `trapframe`。

系统调用约定：

```text
a0-a5  系统调用参数
a7     系统调用号
a0     系统调用返回值
```

对应代码：

- [`user/usys.pl`](../user/usys.pl) 生成用户态 syscall stub，把系统调用号放到 `a7`，执行 `ecall`。
- [`kernel/syscall.c`](../kernel/syscall.c) 从 `p->trapframe->a7` 取系统调用号，从 `a0-a5` 取参数。

## 4. PC：程序计数器

PC 不是普通 GPR，不能像 `x1`、`x2` 那样用普通指令直接读写，但它是处理器核心执行状态。

可操作性：

| 模式 | 可见性 | 直接读写 | 操作方式 |
|---|---:|---:|---|
| U-mode | 间接可见 | 否 | 跳转、调用、返回、trap |
| S-mode | 间接可见 | 否 | 跳转、调用、写 `sepc` 后 `sret` |
| M-mode | 间接可见 | 否 | 跳转、调用、写 `mepc` 后 `mret` |

trap 发生时，当前 PC 会保存到对应 `epc`：

```text
进入 M-mode trap -> mepc
进入 S-mode trap -> sepc
```

xv6 中，用户态系统调用时：

```c
p->trapframe->epc = r_sepc();
p->trapframe->epc += 4;
```

`epc += 4` 是为了跳过 `ecall` 指令。如果不加，返回用户态后会再次执行同一条 `ecall`，导致系统调用无限重复。

## 5. 浮点寄存器 FPR

如果处理器实现 `F/D` 扩展，会有：

```text
f0 - f31
fcsr
```

`RV64GC` 中 `G = I + M + A + F + D`，因此从 ISA 目标上看支持浮点。但 xv6 基本不使用浮点寄存器。

可见性和访问性：

| 模式 | 可见 | 可操作 | 限制 |
|---|---:|---:|---|
| U-mode | 是 | 条件允许 | 受 `mstatus/sstatus.FS` 控制 |
| S-mode | 是 | 条件允许 | OS 可启用/禁用 |
| M-mode | 是 | 条件允许 | 最高权限控制 |

关键点：

- `FS=Off` 时执行浮点指令会触发 illegal instruction。
- OS 可以利用 `FS` 状态做 lazy FPU save/restore。
- xv6 教学内核通常避免使用浮点，简化上下文切换。

## 6. 向量寄存器

如果实现 RISC-V `V` 扩展，会有：

```text
v0 - v31
vtype
vl
vstart
vxrm
vxsat
vcsr
```

但 `RV64GC` 不包含 `V` 扩展，所以本项目不依赖向量寄存器。

可见性类似浮点：

| 模式 | 可见 | 可操作 | 限制 |
|---|---:|---:|---|
| U-mode | 如果实现 V | 条件允许 | 受 `mstatus/sstatus.VS` 控制 |
| S-mode | 如果实现 V | 条件允许 | OS 控制保存恢复 |
| M-mode | 如果实现 V | 条件允许 | 最高权限控制 |

## 7. CSR 总览

CSR 是 Control and Status Registers，即控制状态寄存器。它们负责控制特权级、中断、异常、页表、计数器、物理内存保护等。

按前缀大致分：

```text
m*  Machine-level CSR
s*  Supervisor-level CSR
u*  User-level CSR
h*  Hypervisor-level CSR，可选
```

访问规则：

| CSR 类型 | U-mode | S-mode | M-mode |
|---|---:|---:|---:|
| U-level CSR | 部分可访问 | 可访问 | 可访问 |
| S-level CSR | 不可访问 | 可访问 | 可访问 |
| M-level CSR | 不可访问 | 不可访问 | 可访问 |

还要注意：

- 某些 CSR 只读。
- 某些 CSR 的低权限访问受 `mcounteren`、`scounteren` 等控制。
- 某些 CSR 是可选扩展提供的，具体处理器不一定实现。

## 8. Machine-level CSR

Machine-level CSR 只能由 M-mode 直接访问。xv6 主要在 [`kernel/start.c`](../kernel/start.c) 中使用它们完成启动过渡。

### `mhartid`

当前 hart id。

访问：

| 模式 | 访问权限 |
|---|---|
| M-mode | 可读 |
| S-mode | 不可直接访问 |
| U-mode | 不可直接访问 |

xv6 用法：

- [`kernel/entry.S`](../kernel/entry.S) 读取 `mhartid`，为每个 hart 设置不同启动栈。
- [`kernel/start.c`](../kernel/start.c) 读取 `mhartid` 后写入 `tp`。

```asm
csrr a1, mhartid
```

```c
int id = r_mhartid();
w_tp(id);
```

xv6 后续用 `tp` 保存 hart id，而不是反复读 `mhartid`。

### `mstatus`

Machine status register。控制 M-mode 状态和 `mret` 行为。

xv6 关注位：

```text
MPP  mret 返回后的特权级
```

xv6 启动时：

```c
unsigned long x = r_mstatus();
x &= ~MSTATUS_MPP_MASK;
x |= MSTATUS_MPP_S;
w_mstatus(x);
```

含义：把 `MPP` 设置为 Supervisor mode，之后执行 `mret` 会进入 S-mode。

### `mepc`

Machine exception program counter。`mret` 返回时跳转到 `mepc`。

xv6 启动时：

```c
w_mepc((uint64)main);
asm volatile("mret");
```

含义：`mret` 后进入 `kernel/main.c:main()`。

### `medeleg`

Machine exception delegation。控制哪些异常委托给 S-mode。

xv6：

```c
w_medeleg(0xffff);
```

含义：把常见异常交给 supervisor 内核处理，而不是留在 M-mode。

### `mideleg`

Machine interrupt delegation。控制哪些中断委托给 S-mode。

xv6：

```c
w_mideleg(0xffff);
```

含义：把常见中断交给 S-mode 处理。

### `mie`

Machine interrupt enable。控制 M-mode 中断使能。

xv6 在 [`kernel/riscv.h`](../kernel/riscv.h) 中封装了：

```c
r_mie()
w_mie()
```

这个版本主要用 S-mode 的 `sie` 管理 supervisor external/timer interrupt。

### `menvcfg`

Machine environment configuration。

xv6 用它开启 SSTC：

```c
w_menvcfg(r_menvcfg() | (1L << 63));
```

SSTC 允许 supervisor mode 使用 `stimecmp` 配置时钟中断。

### `mcounteren`

Machine counter enable。控制低特权级能否访问计数器。

xv6：

```c
w_mcounteren(r_mcounteren() | 2);
```

含义：允许 S-mode 访问 `time`。

### `time`

时间计数器。严格说它属于 counter/timer 相关 CSR。xv6 通过 `r_time()` 读取当前时间。

用法：

```c
w_stimecmp(r_time() + 1000000);
```

### `pmpaddr0` / `pmpcfg0`

Physical Memory Protection 配置寄存器。

xv6：

```c
w_pmpaddr0(0x3fffffffffffffull);
w_pmpcfg0(0xf);
```

含义：允许 S-mode 访问物理内存。如果 PMP 没配好，进入 S-mode 后可能无法访问 RAM 或 MMIO 设备。

## 9. Supervisor-level CSR

Supervisor-level CSR 由 S-mode 内核主要使用，M-mode 也可访问，U-mode 不能直接访问。

### `sstatus`

Supervisor status register。控制 S-mode 状态、中断开关和 `sret` 返回行为。

重要位：

```text
SSTATUS_SPP   Previous Privilege，sret 返回 S-mode 还是 U-mode
SSTATUS_SPIE  Supervisor Previous Interrupt Enable
SSTATUS_SIE   Supervisor Interrupt Enable
```

xv6 返回用户态前：

```c
unsigned long x = r_sstatus();
x &= ~SSTATUS_SPP;
x |= SSTATUS_SPIE;
w_sstatus(x);
```

含义：

- 清除 `SPP`，让 `sret` 返回 U-mode。
- 设置 `SPIE`，让返回用户态后中断可用。

xv6 的 `intr_on()` / `intr_off()` 也通过修改 `sstatus.SIE` 实现。

### `sepc`

Supervisor exception program counter。S-mode trap 返回地址。

用户态 trap 进入内核时，硬件把用户 PC 保存到 `sepc`。

xv6 系统调用路径：

```c
p->trapframe->epc = r_sepc();
p->trapframe->epc += 4;
```

返回用户态前：

```c
w_sepc(p->trapframe->epc);
```

### `scause`

Supervisor cause register。记录 trap 原因。

xv6 常见判断：

```c
if (r_scause() == 8) {
  // system call
}
```

常见值：

```text
8                   U-mode ecall，系统调用
12                  instruction page fault
13                  load page fault
15                  store/AMO page fault
0x8000000000000005  supervisor timer interrupt
0x8000000000000009  supervisor external interrupt
```

用法：

- `scause == 8`：进入 `syscall()`。
- `scause == 13/15`：可能是 lazy allocation 缺页，调用 `vmfault()`。
- 高位为 1：表示 interrupt，而不是同步 exception。

### `stval`

Supervisor trap value。保存 trap 附加信息。

page fault 时：

```text
stval = 触发异常的虚拟地址
```

xv6 lazy allocation：

```c
vmfault(p->pagetable, r_stval(), ...)
```

如果 `stval` 对应地址在进程 `p->sz` 范围内，且尚未映射，`vmfault()` 分配物理页并建立映射。

### `stvec`

Supervisor trap vector。S-mode trap 入口地址。

xv6 有两个入口场景：

```text
内核态 trap -> kernelvec
用户态 trap -> trampoline.S:uservec
```

初始化内核 trap：

```c
w_stvec((uint64)kernelvec);
```

返回用户态前设置用户 trap 入口：

```c
uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
w_stvec(trampoline_uservec);
```

原因：用户态 trap 发生时仍在用户页表下，入口必须位于用户页表也映射的 `TRAMPOLINE` 页面。

### `sscratch`

Supervisor scratch register。S-mode trap handler 可用的临时 CSR。

xv6 在 [`kernel/trampoline.S`](../kernel/trampoline.S) 中用它暂存用户 `a0`：

```asm
csrw sscratch, a0
li a0, TRAPFRAME
...
csrr t0, sscratch
sd t0, 112(a0)
```

原因：

- trap 刚进入时，`a0` 里是用户程序的寄存器值。
- 保存寄存器时，xv6 又需要用 `a0` 指向 `TRAPFRAME`。
- 所以先把用户 `a0` 存进 `sscratch`，再把 `a0` 改成 `TRAPFRAME` 地址。

### `sie`

Supervisor interrupt enable。控制 S-mode 哪些中断可用。

xv6 用到：

```text
SIE_SEIE  supervisor external interrupt
SIE_STIE  supervisor timer interrupt
```

启动时：

```c
w_sie(r_sie() | SIE_SEIE | SIE_STIE);
```

### `sip`

Supervisor interrupt pending。表示哪些 S-mode 中断处于 pending。

xv6 封装了 `r_sip()` / `w_sip()`，主路径更多通过 `scause` 和 PLIC 判断中断来源。

### `satp`

Supervisor address translation and protection。页表根寄存器，是虚拟内存隔离的核心。

访问权限：

| 模式 | 访问 `satp` |
|---|---|
| U-mode | 不可访问 |
| S-mode | 可访问 |
| M-mode | 可访问 |

`satp` 字段包含：

```text
MODE  分页模式，例如 Sv39
ASID  地址空间 ID
PPN   根页表物理页号
```

xv6 使用 Sv39：

```c
#define SATP_SV39 (8L << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))
```

启用内核页表：

```c
w_satp(MAKE_SATP(kernel_pagetable));
```

用户/内核切换时，`trampoline.S` 写 `satp`：

```asm
csrw satp, t1   # 切到内核页表
...
csrw satp, a0   # 切到用户页表
```

写 `satp` 后通常执行：

```asm
sfence.vma
```

用于刷新 TLB，避免旧地址翻译结果继续生效。

### `stimecmp`

Supervisor timer compare。设置下一次 S-mode timer interrupt 的时间点。

xv6 初始化和每次时钟中断后都会设置：

```c
w_stimecmp(r_time() + 1000000);
```

在 [`kernel/trap.c`](../kernel/trap.c) 的 `clockintr()` 中，时钟中断处理完会设置下一次中断。

## 10. User-level CSR

常见 U-level CSR：

```text
ustatus
uie
utvec
uscratch
uepc
ucause
utval
uip
cycle
time
instret
```

访问性：

| 模式 | 可见 | 可读写 |
|---|---:|---:|
| U-mode | 部分可见 | 部分可访问 |
| S-mode | 可访问 | 按 CSR 属性 |
| M-mode | 可访问 | 按 CSR 属性 |

计数器类 CSR 比较特殊：

```text
cycle
time
instret
```

U-mode 是否可以访问，可能受以下寄存器控制：

```text
mcounteren
scounteren
```

xv6 用户程序通常不直接依赖 U-level CSR，而是通过系统调用获取时间，例如：

```text
uptime()
```

## 11. trap 时 CSR 如何配合

用户态系统调用：

```text
U-mode 执行 ecall
  -> 硬件保存用户 PC 到 sepc
  -> 硬件设置 scause = 8
  -> 跳转到 stvec
  -> trampoline.S:uservec
  -> 保存 GPR 到 trapframe
  -> 切换 satp 到内核页表
  -> trap.c:usertrap()
  -> syscall()
  -> prepare_return()
  -> trampoline.S:userret
  -> 切换 satp 到用户页表
  -> sret
  -> 回到 U-mode
```

关键 CSR 角色：

```text
stvec     决定 trap 入口
sepc      保存/恢复用户 PC
scause    告诉内核 trap 原因
stval     保存异常地址，例如 page fault VA
sstatus   决定 sret 返回到 U-mode 还是 S-mode
satp      切换用户/内核页表
sscratch  临时保存用户 a0
```

page fault 路径：

```text
用户访问未映射地址
  -> scause = 13 或 15
  -> stval = fault virtual address
  -> usertrap()
  -> vmfault(pagetable, stval, ...)
  -> 成功则补页
  -> 失败则 setkilled(p)
```

## 12. 各模式访问权限总表

```text
                 U-mode          S-mode          M-mode
GPR x0-x31       可读写          可读写          可读写
PC               间接操作        间接操作        间接操作
FPR f0-f31       条件可用        条件可用        条件可用
Vector v0-v31    条件可用        条件可用        条件可用
U-level CSR      部分可访问      可访问          可访问
S-level CSR      不可访问        可访问          可访问
M-level CSR      不可访问        不可访问        可访问
satp             不可访问        可访问          可访问
pmp*             不可访问        不可访问        可访问
medeleg/mideleg  不可访问        不可访问        可访问
```

更准确地说：

- “可访问”仍受 CSR 自身读写属性限制。
- 低特权级访问高特权级 CSR 会触发 illegal instruction。
- `cycle/time/instret` 这类计数器是否能在 U/S mode 访问，还受 `counteren` 控制。
- FPR/Vector 是否能操作，还受 `FS/VS` 状态控制。

## 13. xv6 最核心的寄存器闭环

xv6 启动阶段：

```text
mhartid  -> 区分 hart
mstatus  -> 设置 mret 目标特权级
mepc     -> 设置 mret 跳转到 main
medeleg  -> 异常委托给 S-mode
mideleg  -> 中断委托给 S-mode
pmp*     -> 允许 S-mode 访问物理内存
menvcfg  -> 启用 SSTC
mcounteren -> 允许 S-mode 读 time
```

xv6 运行阶段：

```text
stvec    -> trap 入口
sepc     -> trap 返回地址
scause   -> trap 原因
stval    -> page fault 地址
sstatus  -> 中断状态和 sret 返回模式
satp     -> 当前页表
sscratch -> trampoline 临时保存 a0
sie      -> S-mode 中断使能
time/stimecmp -> 时钟中断
```

一句话总结：

```text
普通寄存器负责计算和函数调用；
CSR 负责特权级、中断、异常、页表和系统隔离；
xv6 的启动、系统调用、缺页处理和调度时钟都建立在这些 CSR 的协作上。
```
