# RISC-V 64 寄存器与特权级访问指南

本文总结 RISC-V 64，尤其是 xv6-riscv 项目中涉及的寄存器分类、各特权级对寄存器的可见性/可访问性/可操作性，以及这些寄存器在 xv6 启动、trap、页表切换、时钟中断中的实际用法。

相关源码：

- [`kernel/riscv.h`](../kernel/riscv.h)：xv6 对 RISC-V CSR 和页表宏的封装。
- [`kernel/entry.S`](../kernel/entry.S)：启动入口，读取 `mhartid`，设置早期栈。
- [`kernel/start.c`](../kernel/start.c)：machine mode 初始化，切换到 supervisor mode。
- [`kernel/trap.c`](../kernel/trap.c)：trap、系统调用、中断、page fault 处理。
- [`kernel/trampoline.S`](../kernel/trampoline.S)：用户态/内核态切换时保存寄存器和切换页表。
- [`kernel/vm.c`](../kernel/vm.c)：写 `satp` 启用 Sv39 页表。
- [`kernel/plic.c`](../kernel/plic.c)：PLIC 外部中断控制器初始化、claim、complete。
- [`kernel/memlayout.h`](../kernel/memlayout.h)：QEMU `virt` 平台上的 PLIC、CLINT、UART、virtio MMIO 地址。

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

## 3. 不同模式下寄存器的同一性

讨论“不同模式下的同一个寄存器是否相同”时，要先区分两类寄存器：

```text
普通架构寄存器，例如 x0-x31、FPR、Vector
控制状态寄存器 CSR，例如 mepc/sepc、mstatus/sstatus、satp
```

结论是：

```text
普通计算寄存器基本是跨模式同一组；
CSR 大多按特权级分层，不是简单的同一组；
少数状态 CSR 之间存在字段级视图或关联。
```

### 3.1 GPR 是同一组

RISC-V 的通用寄存器：

```text
x0-x31
ra/sp/gp/tp/t0/a0/s0/...
```

在 U-mode、S-mode、M-mode 下是同一组架构寄存器。也就是说：

```text
U-mode 的 a0
S-mode 的 a0
M-mode 的 a0
```

本质上都是 `x10`，不是每个模式各有一份。

因此 trap 发生时，如果内核想保留用户态的 `a0`、`sp`、`ra` 等值，必须由软件保存。RISC-V 硬件不会自动给每个模式准备一套 GPR，也不会自动保存所有 GPR。

xv6 在 [`kernel/trampoline.S`](../kernel/trampoline.S) 中手动保存用户寄存器：

```asm
sd ra, 40(a0)
sd sp, 48(a0)
...
sd a7, 168(a0)
```

这正是因为用户态和内核态共用同一组 GPR。进入内核后，内核代码会继续使用这些寄存器，如果不先保存，用户上下文就会被破坏。

### 3.2 `sp` 不是每个模式独立

`sp` 是 `x2`，也属于 GPR，所以不同模式下不是各有一份。

但是操作系统会在模式切换时主动切换栈：

```text
用户态运行时：
  sp 指向用户栈

trap 进入内核后：
  trampoline 保存用户 sp
  再把 sp 改成当前进程的内核栈
```

xv6 在 `trampoline.S` 中保存用户 `sp` 到 trapframe，之后加载内核栈：

```asm
sd sp, 48(a0)
...
ld sp, 8(a0)
```

所以应区分：

```text
硬件寄存器 sp 只有一份；
用户栈和内核栈是两块不同内存；
OS 通过保存/恢复 sp 在两块栈之间切换。
```

### 3.3 `tp` 在 xv6 中也不是每模式一份

`tp` 是 `x4`，也是同一组 GPR。

xv6 启动时把 hart id 写入 `tp`：

```c
int id = r_mhartid();
w_tp(id);
```

内核后续用 `tp` 实现 `cpuid()`。但用户程序也可能使用或修改 `tp`。因此 xv6 在用户态 trap 进入内核时保存用户 `tp`，并重新加载内核需要的 hart id：

```asm
sd tp, 64(a0)      # 保存用户 tp
ld tp, 32(a0)      # 加载 kernel_hartid 到 tp
```

这说明 `tp` 并不是 U-mode 一份、S-mode 一份；只是 xv6 在不同上下文中约定了不同用途。

### 3.4 PC 是一份执行状态，但返回地址 CSR 分层

PC 不是普通 GPR，但它也是当前 hart 的执行状态，不是 U/S/M 各一套。

trap 发生时，硬件会把当前 PC 保存到对应的 exception program counter：

```text
进入 M-mode trap -> mepc
进入 S-mode trap -> sepc
```

之后 PC 被设置为 trap vector，例如 `mtvec` 或 `stvec` 指向的地址。返回时：

```text
mret 从 mepc 恢复 PC
sret 从 sepc 恢复 PC
```

所以：

```text
PC 本身不是每个模式独立一份；
但不同特权级有不同的返回地址 CSR，例如 mepc、sepc。
```

### 3.5 CSR 大多不是同一组

CSR 和 GPR 不同。很多 CSR 按特权级分层，有不同名字和不同用途：

```text
mstatus / sstatus / ustatus
mepc    / sepc    / uepc
mcause  / scause  / ucause
mtval   / stval   / utval
mtvec   / stvec   / utvec
mie     / sie     / uie
mip     / sip     / uip
```

这些通常不能简单理解成“同一个寄存器在不同模式下的别名”。例如：

```text
mepc 和 sepc 是两个不同 CSR
mcause 和 scause 是两个不同 CSR
mtvec 和 stvec 是两个不同 CSR
mscratch 和 sscratch 是两个不同 CSR
```

M-mode trap 使用 `mepc/mcause/mtval/mtvec`；S-mode trap 使用 `sepc/scause/stval/stvec`。S-mode 写 `sepc` 不等价于写 `mepc`，S-mode 设置 `stvec` 也不会影响 `mtvec`。

### 3.6 部分 CSR 是受限视图或字段级关联

有些 S-level CSR 可以看作 M-level CSR 中相关字段的受限视图，最典型的是：

```text
sstatus  和 mstatus
sie      和 mie
sip      和 mip
```

更准确地说：

```text
sstatus 暴露 mstatus 中和 S/U mode 相关的字段
sie     暴露 mie 中 supervisor interrupt enable 相关位
sip     暴露 mip 中 supervisor interrupt pending 相关位
```

所以这些寄存器之间不是完全无关的两份状态，但也不能简单说“同一个寄存器”。它们是字段级关联或受限视图。

### 3.7 同一性总结

```text
类型                         不同模式是否同一份
GPR x0-x31                   是，同一组
sp/ra/a0/tp 等 ABI 寄存器     是，本质都是 x 寄存器
PC                           当前执行状态一份，但 trap 保存到不同 epc
FPR f0-f31                   是，同一组，若启用
Vector v0-v31                是，同一组，若实现
mepc/sepc/uepc               否，不同 CSR
mcause/scause/ucause         否，不同 CSR
mtvec/stvec/utvec            否，不同 CSR
mscratch/sscratch/uscratch   否，不同 CSR
mstatus/sstatus/ustatus      不是简单同一份，存在字段级视图/关联
mie/sie/uie                  存在字段级关联
mip/sip/uip                  存在字段级关联
satp                         S-level CSR，不是 U/M 各一份
```

一句话概括：

```text
计算用的寄存器，例如 x0-x31，是跨模式同一组；
控制系统行为的 CSR，大多按特权级分层，不是同一个；
少数状态 CSR 之间存在字段级映射或受限视图关系。
```

这也是为什么 xv6 进入内核时必须手动保存用户寄存器：用户态和内核态没有自动隔离的一套 GPR。

## 4. 通用寄存器 GPR

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

## 5. PC：程序计数器

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

## 6. 浮点寄存器 FPR

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

## 7. 向量寄存器

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

## 8. CSR 总览

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

## 9. Machine-level CSR

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

## 10. Supervisor-level CSR

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

## 11. PLIC、CLINT 与 SSTC

PLIC、CLINT、SSTC 都和 RISC-V 平台中断有关，但它们不是同一层面的东西：

```text
PLIC   Platform-Level Interrupt Controller，平台级外部中断控制器
CLINT  Core-Local Interruptor，核心本地中断控制器
SSTC   Supervisor-mode Timer Compare，S-mode timer compare 扩展
```

在 QEMU `virt` 机器上，相关 MMIO 地址写在 [`kernel/memlayout.h`](../kernel/memlayout.h)：

```text
0x02000000  CLINT
0x0c000000  PLIC
0x10000000  UART0
0x10001000  virtio disk
```

本项目中的实际使用情况是：

```text
外部设备中断：走 PLIC
时钟中断：走 time/stimecmp/SSTC
CLINT 地址：保留在 memlayout.h，但主 timer 路径未直接操作传统 CLINT mtimecmp
```

### 11.1 PLIC：平台级外部中断控制器

PLIC 负责外部设备中断，例如：

```text
UART 输入中断
virtio disk 完成中断
```

外设产生中断后，PLIC 负责记录 pending、比较优先级、检查 enable 位和 threshold，并把 external interrupt 送给对应 hart。

xv6 的 PLIC 代码在 [`kernel/plic.c`](../kernel/plic.c)。

全局初始化：

```c
void
plicinit(void)
{
  *(uint32 *)(PLIC + UART0_IRQ * 4) = 1;
  *(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;
}
```

含义：

```text
把 UART 和 virtio disk 的 IRQ 优先级设置为 1
优先级为 0 表示该中断源禁用
```

每个 hart 初始化：

```c
void
plicinithart(void)
{
  int hart = cpuid();

  *(uint32 *)PLIC_SENABLE(hart) =
    (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  *(uint32 *)PLIC_SPRIORITY(hart) = 0;
}
```

含义：

```text
允许当前 hart 的 S-mode 接收 UART 和 virtio disk 中断
把当前 hart 的 S-mode priority threshold 设置为 0
```

外部中断处理路径：

```text
UART / virtio disk
  -> PLIC
  -> supervisor external interrupt
  -> scause = 0x8000000000000009
  -> trap.c:devintr()
  -> plic_claim()
  -> uartintr() 或 virtio_disk_intr()
  -> plic_complete()
```

xv6 在 [`kernel/trap.c`](../kernel/trap.c) 中判断 external interrupt：

```c
if (scause == 0x8000000000000009L) {
  int irq = plic_claim();

  if (irq == UART0_IRQ) {
    uartintr();
  } else if (irq == VIRTIO0_IRQ) {
    virtio_disk_intr();
  }

  if (irq)
    plic_complete(irq);

  return 1;
}
```

`plic_claim()` 的作用是从 PLIC 取出当前 hart 应处理的 IRQ；`plic_complete(irq)` 告诉 PLIC 该 IRQ 已经处理完成，之后该设备才能继续产生同类中断。

### 11.2 CLINT：核心本地中断控制器

CLINT 传统上负责每个 hart 的本地中断：

```text
timer interrupt
software interrupt
```

常见 CLINT 寄存器包括：

```text
msip       software interrupt pending
mtime      全局时间计数器
mtimecmp   每个 hart 的 timer compare
```

它和 PLIC 的职责差异：

```text
PLIC   处理平台外设中断，来自 UART、磁盘等外设
CLINT  处理 hart-local 中断，主要是 timer 和 software interrupt
```

本项目在 [`kernel/memlayout.h`](../kernel/memlayout.h) 中保留了 CLINT 地址宏：

```c
#define CLINT_BASE  0x02000000L
#define CLINT(hart) (CLINT_BASE + (hart) * 4)
```

但当前 timer 主路径并没有直接写传统 CLINT 的 `mtimecmp` MMIO，而是使用 SSTC 提供的 `stimecmp` CSR。这是和一些早期 xv6-riscv 版本不同的地方。

### 11.3 SSTC：S-mode timer compare

SSTC 让 supervisor mode 可以直接使用 `stimecmp` 设置 S-mode timer interrupt 的下一次触发时间。

xv6 在 [`kernel/start.c`](../kernel/start.c) 的 `timerinit()` 中启用 SSTC：

```c
w_menvcfg(r_menvcfg() | MENVCFG_STCE);
w_mcounteren(r_mcounteren() | 2);
w_stimecmp(r_time() + 1000000);
```

含义：

```text
menvcfg.STCE      启用 S-mode timer compare
mcounteren bit 1  允许 S-mode 读取 time
stimecmp          设置第一次 timer interrupt
```

timer interrupt 到期后，`scause` 为：

```text
0x8000000000000005  supervisor timer interrupt
```

在 [`kernel/trap.c`](../kernel/trap.c) 的 `devintr()` 中：

```c
} else if (scause == 0x8000000000000005L) {
  clockintr();
  return 2;
}
```

`clockintr()` 会更新时间并设置下一次 timer interrupt：

```c
w_stimecmp(r_time() + 1000000);
```

如果这个 timer interrupt 来自用户进程运行期间，`usertrap()` 后续会：

```c
if (which_dev == 2)
  yield();
```

这就是 xv6 抢占式调度的时钟基础。

### 11.4 PLIC、CLINT、SSTC 对比

| 模块 | 职责 | 典型中断 | 本项目使用情况 |
|---|---|---|---|
| PLIC | 平台级外部中断控制器 | UART、virtio disk | 实际使用，见 `plic.c` 和 `devintr()` |
| CLINT | hart 本地中断控制器 | timer、software interrupt | 保留地址宏，当前主 timer 路径不直接操作传统 `mtimecmp` |
| SSTC/stimecmp | S-mode timer compare | supervisor timer interrupt | 实际使用，见 `timerinit()` 和 `clockintr()` |

完整中断路径可以概括为：

```text
外部设备中断：
  UART / virtio disk
    -> PLIC
    -> scause = supervisor external interrupt
    -> devintr()
    -> plic_claim()
    -> uartintr() / virtio_disk_intr()
    -> plic_complete()

时钟中断：
  time >= stimecmp
    -> scause = supervisor timer interrupt
    -> devintr()
    -> clockintr()
    -> ticks++
    -> w_stimecmp(r_time() + 1000000)
    -> yield() if running user process
```

一句话总结：

```text
PLIC 管“外设打断 CPU”；
CLINT 传统上管“每个 CPU 自己的 timer/software interrupt”；
这份 xv6 的外设中断走 PLIC，时钟中断走 SSTC 的 time/stimecmp。
```

## 12. User-level CSR

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

## 13. trap 时 CSR 如何配合

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

## 14. 各模式访问权限总表

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

## 15. xv6 最核心的寄存器闭环

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
