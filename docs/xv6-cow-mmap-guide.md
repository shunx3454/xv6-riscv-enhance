# xv6 COW、Lazy Allocation 与 mmap 实现指南

本文总结当前项目中 Copy-on-Write、惰性堆分配和 mmap 的实际实现，重点说明虚拟内存状态、物理页生命周期、缺页处理、fork、内核用户拷贝、锁与失败回滚。文末给出整个 xv6 功能增强项目的简历描述和面试讲解框架。

本文描述的是当前仓库的实现，不等同于完整 Linux 语义。

## 1. 当前实现范围

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 普通用户页 fork COW | 已实现 | 可写页共享，只在首次写入时复制 |
| 物理页引用计数 | 已实现 | `kalloc/kfree/uvmunmap` 统一维护 |
| Lazy `sbrk` | 已实现 | 只增大 `p->sz`，首次访问分配零页 |
| 文件 `MAP_PRIVATE` | 已实现 | 惰性装页，fork 后可写驻留页使用 COW，不写回文件 |
| 文件 `MAP_SHARED` | 简化实现 | fork 时已驻留页直接共享，`munmap/exit/exec` 时写回 |
| `MAP_PRIVATE | MAP_ANONYMOUS` | 已实现 | 无文件后端，惰性分配零页，fork 后使用 COW |
| `MAP_SHARED | MAP_ANONYMOUS` | 未实现 | `sys_mmap()` 明确拒绝 |
| 非零文件 offset | 未实现 | 当前要求 `offset == 0` |
| 指定 mmap 地址、`MAP_FIXED` | 未实现 | 当前要求 `addr == 0`，内核 first-fit 选址 |
| 部分 `munmap` | 未实现 | 只允许从 VMA 起点解除整段映射，`len` 暂时忽略 |
| `PROT_EXEC`、`PROT_NONE` | 未实现 | 只支持至少包含 READ 或 WRITE 的映射 |
| page cache、dirty tracking、`msync` | 未实现 | `MAP_SHARED` 不具备 Linux 的全局一致性 |

## 2. 相关模块

| 文件 | 主要职责 |
| --- | --- |
| `kernel/riscv.h` | Sv39 PTE 标志和软件位 `PTE_COW` |
| `kernel/memlayout.h` | `MMAPBASE`、`TRAPFRAME`、`TRAMPOLINE` 地址布局 |
| `kernel/kalloc.c` | 物理页分配、释放和引用计数 |
| `kernel/proc.h` | `struct vma` 和每进程 VMA 表 |
| `kernel/proc.c` | `fork`、`exit`、普通地址空间边界和失败清理 |
| `kernel/vm.c` | 页表、COW、统一缺页、mmap 装页、VMA fork/unmap |
| `kernel/trap.c` | load/store page fault 入口 |
| `kernel/sysproc.c` | eager/lazy `sbrk` |
| `kernel/sysfile.c` | `mmap/munmap`，文件 I/O 前的 mmap prefault |
| `kernel/file.c` | 使用显式文件偏移的 `filewriteat()` |
| `kernel/exec.c` | 成功 exec 后清理旧 VMA |
| `user/cowmmaptest.c` | COW、文件 mmap、匿名 mmap 和生命周期测试 |

## 3. Sv39 和用户地址空间

RISC-V Sv39 使用三级页表，每级索引 9 位，页内偏移 12 位，基础页大小为 4096 字节。

常用 PTE 位：

```c
#define PTE_V   (1L << 0)
#define PTE_R   (1L << 1)
#define PTE_W   (1L << 2)
#define PTE_X   (1L << 3)
#define PTE_U   (1L << 4)
#define PTE_COW (1L << 8)
```

`PTE_COW` 使用 RISC-V PTE 的 RSW 软件保留位。硬件不解释这个位，内核用它区分两种只读页面：

```text
普通只读页：写入是保护错误，应杀死进程
COW 只读页：原本可写，因为共享而暂时只读，可以复制后恢复写权限
```

当前用户地址空间划分为：

```text
0
│ ELF text / data / bss
│ user stack
│ heap
│ p->sz
│
├──────────────────────── MMAPBASE = MAXVA / 2
│ mmap VMA
│ 空洞
│ mmap VMA
│ ...
├──────────────────────── TRAPFRAME
│ trapframe
├──────────────────────── TRAMPOLINE
```

必须保持两个边界条件：

```c
p->sz <= MMAPBASE
MMAPBASE <= vma->addr
vma->addr + vma->maplen <= TRAPFRAME
```

这样普通堆和 mmap 区域不会碰撞。`exec()` 加载 ELF、eager/lazy `sbrk()` 都会检查 `MMAPBASE`。

## 4. 统一页面状态模型

当前实现把普通页、COW、lazy heap 和 mmap 汇合到 `vmfault()`：

```text
访问用户虚拟地址
        │
        ├── PTE 有效
        │     ├── 权限满足：直接访问
        │     ├── 写访问 + PTE_COW：cow_break()
        │     └── 权限不满足：失败
        │
        └── PTE 不存在
              ├── va < p->sz 且 va < MMAPBASE：分配 lazy heap 零页
              ├── 地址属于 VMA：按 VMA 类型装页
              └── 其他地址：失败
```

这里有两套互补的事实来源：

- VMA 判断“这个地址是否属于映射、允许什么访问、是否有文件后端”。
- PTE 判断“页面当前是否驻留、硬件权限是什么、是否处于 COW 状态”。

不需要额外增加 `PTE_MMAP`。否则 VMA 和 PTE 会保存重复状态，容易产生不一致。

## 5. 物理页引用计数

COW 和共享 mmap 都允许多个 PTE 指向同一个物理页，因此不能继续使用“解除一个映射就立即释放页面”的模型。

当前分配器维护：

```c
refcnt[(PHYSTOP - KERNBASE) / PGSIZE]
```

物理地址通过以下公式转换为数组索引：

```c
#define PAINDEX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
```

引用计数规则：

```text
kalloc()           空闲页 0 -> 1
kaddref(pa)        新增一个 PTE/所有者引用
kfree(pa)          引用数减 1
refcnt 降为 0      页面填充垃圾值并放回 freelist
uvmunmap(do_free=1) 通过 kfree() 释放映射引用
```

`kmem.lock` 同时保护 freelist 和引用计数，保证多核下计数变化与页面回收是原子的。

初始化时 `freerange()` 先把待释放页计数设为 1，再调用 `kfree()` 变成 0 并放入 freelist。这避免初始化路径绕开统一的释放规则。

必须保持以下不变量：

```text
refcnt == 0  <=> 页面位于 freelist 或尚未分配
refcnt > 0   => 页面不能进入 freelist
每增加一个共享映射，必须先 kaddref()
每删除一个映射，必须恰好执行一次 kfree()
```

## 6. fork 的 COW 实现

### 6.1 `uvmcopy()` 不再复制物理页

传统 fork 对每个用户页执行：

```text
kalloc + memmove + mappages
```

当前 `uvmcopy(old, new, sz)` 改为共享物理页：

1. 遍历 `[0, sz)` 的每个页面。
2. 跳过不存在或无效的 PTE，兼容 lazy heap。
3. 取得父 PTE 的 PA 和 flags。
4. 原本可写的页清除 `PTE_W`，设置 `PTE_COW`。
5. 修改父 PTE，并使用相同 flags 建立子 PTE。
6. 调用 `kaddref(pa)` 墕加物理页引用。
7. 如果修改过父 PTE，执行 `sfence_vma()` 刷新 TLB。

示意：

```text
fork 前：
父 VA ── RW ──> 物理页 P，ref=1

fork 后：
父 VA ── R+COW ─┐
                 ├──> 物理页 P，ref=2
子 VA ── R+COW ─┘
```

只给原本可写的页设置 COW。代码页和其他真正只读页可以共享，但不能设置 `PTE_COW`，否则非法写入会被错误地允许。

### 6.2 为什么必须刷新 TLB

父页表中的 `PTE_W` 已经被清除，但 CPU 可能仍缓存旧的可写翻译。如果不执行 `sfence_vma()`，父进程可能继续写共享页而不产生 page fault，直接破坏子进程快照。

### 6.3 `cow_break()` 的快路径和慢路径

store page fault 进入 `vmfault(..., VM_WRITE)`，发现 `PTE_COW` 后调用 `cow_break()`。

如果引用计数为 1：

```text
已经没有其他映射共享该页
不需要复制
清除 PTE_COW，恢复 PTE_W
刷新 TLB
```

如果引用计数大于 1：

```text
kalloc 新页
复制完整 4096 字节
当前 PTE 改指向新页
清除 PTE_COW，恢复 PTE_W
kfree(oldpa) 递减旧页引用
刷新 TLB
```

示意：

```text
写入前：父 ─┐
             ├──> P
        子 ─┘

子写入后：父 ───> P
          子 ───> P2（P 的副本）
```

必须先成功分配和复制新页，再替换 PTE、减少旧页引用。否则失败路径可能使当前进程丢失原映射。

### 6.4 fork 失败回滚

如果构造子页表失败：

- 解除已经建立的子 PTE，递减相应引用计数。
- 父进程已经改成 COW 的页面可以保持 COW。
- 父进程以后写该页时，如果引用数已经回到 1，`cow_break()` 会走快路径恢复写权限。

不需要复杂地把父页表逐项恢复为原状态。

## 7. Lazy `sbrk`

用户库提供 eager 和 lazy 两种增长方式：

```c
sbrk(n);      // SBRK_EAGER
sbrklazy(n);  // SBRK_LAZY
```

lazy 增长只执行：

```c
p->sz += n;
```

不调用 `kalloc()`，不建立 PTE。首次访问满足：

```c
va < p->sz && va < MMAPBASE
```

时，`vmfault()` 分配清零页面并建立：

```c
PTE_R | PTE_W | PTE_U
```

fork 时不存在的 lazy 页被 `uvmcopy()` 跳过。父子以后分别访问时各自获得独立零页，这符合普通私有地址空间语义。

lazy allocation 带来的连锁修改包括：

- `uvmunmap()` 必须允许区间中存在尚未分配的页。
- `uvmcopy()` 必须跳过 PTE 空洞。
- `copyin/copyout/copyinstr` 必须能触发统一缺页处理。
- `sbrk`、ELF 和 mmap 必须共享清晰的地址边界。

## 8. VMA 数据结构

每个进程有固定大小的 VMA 表：

```c
#define NVMA 8

struct vma {
  int used;
  uint64 addr;
  uint64 len;
  uint64 maplen;
  int prot;
  int flags;
  struct file *file;
  uint64 offset;
};
```

字段语义：

- `addr`：页对齐后的映射起始地址。
- `len`：用户请求的真实字节长度。
- `maplen`：`PGROUNDUP(len)`，页表映射和区间查找使用。
- `prot`：当前只保留 `PROT_READ/PROT_WRITE`。
- `flags`：`MAP_PRIVATE/MAP_SHARED`，可附加 `MAP_ANONYMOUS`。
- `file`：文件映射持有独立 `file` 引用；匿名映射为 0。
- `offset`：保留文件偏移字段，当前要求为 0。

必须区分 `len` 和 `maplen`：

```text
建立/解除页表映射：使用 maplen
最后一页文件读取/写回：使用 len 限制有效字节数
```

## 9. `sys_mmap()`

### 9.1 参数验证

当前公共限制：

```text
addr == 0
offset == 0
len > 0
prot 至少包含 PROT_READ 或 PROT_WRITE
映射类型必须且只能选择 MAP_PRIVATE 或 MAP_SHARED
不接受未知 flags
```

文件映射还要求：

```text
fd 有效
file->type == FD_INODE
file->readable
MAP_SHARED + PROT_WRITE 时 file->writable
```

私有可写文件映射可以使用只读 fd，因为修改不会写回原文件。

匿名映射只接受：

```c
MAP_PRIVATE | MAP_ANONYMOUS
fd == -1
offset == 0
```

`MAP_ANON` 是 `MAP_ANONYMOUS` 的别名。`MAP_SHARED | MAP_ANONYMOUS` 当前返回失败。

### 9.2 地址选择

在 `[MMAPBASE, TRAPFRAME)` 中使用 first-fit：

1. 从 `MMAPBASE` 作为候选地址。
2. 与最多 8 个已有 VMA 检查重叠。
3. 重叠时移动到对应 VMA 末尾。
4. 没有重叠时使用该空洞。
5. 没有空槽或地址空间不足时失败。

由于 `NVMA` 很小，O(N²) 扫描足够简单且可接受。

### 9.3 mmap 本身只登记 VMA

成功时不分配物理页，也不建立叶 PTE：

```text
文件映射：v->file = filedup(f)
匿名映射：v->file = 0
```

文件引用独立于文件描述符表，因此 mmap 后关闭 fd 不会使映射失效。

## 10. mmap 缺页装入

`mmap_fault_page()` 首先执行：

```c
mem = kalloc();
memset(mem, 0, PGSIZE);
```

清零是必要的：

- 匿名映射必须初始为零。
- 文件最后一页可能不足 4096 字节。
- 文件可能短读或提前到达 EOF。
- 未从文件覆盖的页尾不能暴露旧物理页数据。

### 10.1 文件映射

当前页面相对 VMA 的偏移：

```c
pageoff = va0 - v->addr;
fileoff = v->offset + pageoff;
n = min(PGSIZE, v->len - pageoff);
```

随后在 inode 锁保护下执行 `readi()`，最后根据 VMA 权限建立 PTE。

### 10.2 私有匿名映射

匿名映射跳过 inode 和 `readi()`，直接把清零页映射到当前虚拟地址：

```text
第一次读取 -> 得到 0
第一次写入 -> 分配零页后重新执行写指令
```

它不是文件映射到 `/dev/zero`，而是 VMA 没有文件后端。

### 10.3 RISC-V 的写权限限制

RISC-V 中 `W=1,R=0` 是保留的非法叶 PTE 编码。因此当前实现遇到 `PROT_WRITE` 时设置：

```c
PTE_R | PTE_W | PTE_U
```

也就是说用户请求 write-only 时，硬件页表实际是 read-write。若要严格实现 write-only，应在 syscall 层拒绝这种组合或引入额外软件检查。

## 11. `vmfault()` 的判定顺序

统一缺页处理必须先看有效 PTE，再看地址区间：

```c
va0 = PGROUNDDOWN(va);
pte = walk(p->pagetable, va0, 0);

if (pte 有效) {
  检查 PTE_U;
  if (写访问 && PTE_COW)
    return cow_break(...);
  检查 PTE_R/PTE_W;
  return 0;
}

if (va 位于低地址 lazy heap)
  分配匿名零页;

v = vma_find(p, va);
if (v 不存在或 prot 不允许访问)
  return -1;

return mmap_fault_page(...);
```

判定顺序不能颠倒：

- 不能把所有 store page fault 都当成 COW。
- 不能仅根据高地址就认为它属于 mmap，必须找到有效 VMA。
- 不能只根据 VMA 权限忽略已经建立 PTE 的 COW 状态。

`usertrap()` 将 `scause=13` 作为读缺页，将 `scause=15` 作为写缺页。处理失败时设置 killed，进程以错误状态退出。当前没有处理 instruction page fault，因此 mmap 页面不可执行。

## 12. 内核 `copyin/copyout/copyinstr`

这是实现 COW 和 mmap 时最容易遗漏的路径。

用户态写 COW 页会由 CPU 产生 store page fault，但 xv6 的 `copyout()` 并不是使用用户页表直接访问用户虚拟地址。它先 walk 得到 PA，再通过内核直接映射执行 `memmove()`，因此不会自然产生用户页表写保护异常。

所以每处理一个用户页面前必须显式调用：

```text
copyout()   -> vmfault(..., VM_WRITE)
copyin()    -> vmfault(..., VM_READ)
copyinstr() -> vmfault(..., VM_READ)
```

这同时处理：

- copyout 断开 COW。
- 为 lazy heap 分配页面。
- 为尚未驻留的 mmap 地址装页。
- 拒绝只读或非法用户地址。

随后仍要重新 walk 并检查最终 PTE 权限，不能把 `vmfault()` 当成地址转换函数。

当前接口只有 `pagetable_t`，因此通过以下条件确认它是当前进程页表：

```c
p != 0 && pagetable == p->pagetable
```

这样 `exec()` 构造临时页表时不会错误地使用当前进程 VMA。

## 13. `vmfault_range()` 和锁上下文

文件 mmap 缺页可能执行：

```text
ilock -> readi -> buffer cache / disk I/O -> sleep
```

某些系统调用会在持有 pipe 自旋锁、inode 锁、`wait_lock` 或子进程锁时执行 `copyin/copyout`。如果这时才触发文件 mmap 缺页，可能出现：

- 持有自旋锁睡眠。
- 对同一个 inode 重复加锁。
- 锁顺序反转和死锁。

因此当前项目在进入这些路径前调用 `vmfault_range()`，预先处理缓冲区范围中的缺失 VMA 页。调用点包括：

```text
sys_read   VM_WRITE
sys_write  VM_READ
sys_fstat  VM_WRITE
sys_pipe   VM_WRITE
kwait      VM_WRITE
```

方向是以内核对用户缓冲区的访问为准：

```text
read(fd, user_buf)  内核写 user_buf -> VM_WRITE
write(fd, user_buf) 内核读 user_buf -> VM_READ
```

它是一个 prefault 安全层，不是最终地址合法性检查。最终检查仍由 `copyin/copyout` 完成。

当前 `mmap_fault_page()` 还会通过 `holdingsleep()` 避免重复获取已经持有的 inode 睡眠锁。更完整的内核应建立统一 uaccess 规则和严格锁顺序，而不是依赖分散的预处理。

## 14. VMA 在 fork 中的处理

低地址普通空间由 `uvmcopy()` 处理，高地址 VMA 由 `vma_fork()` 单独处理。

每个有效 VMA：

```text
复制元数据
文件 VMA：filedup(src->file)
匿名 VMA：file 保持为 0
遍历已经驻留的页面
```

处理矩阵：

| VMA/PTE 类型 | fork 处理 |
| --- | --- |
| 未驻留的私有页 | 子进程保持无 PTE，以后独立缺页 |
| `MAP_PRIVATE` 可写驻留页 | 父子清 W、设置 COW、共享 PA |
| `MAP_PRIVATE` 只读驻留页 | 共享只读 PA |
| `MAP_SHARED` 可写驻留页 | 直接共享可写 PA，不设置 COW |
| `MAP_SHARED` 只读驻留页 | 直接共享只读 PA |

必须使用位判断：

```c
src->flags & MAP_PRIVATE
```

不能继续使用：

```c
src->flags == MAP_PRIVATE
```

因为匿名私有映射的 flags 是 `MAP_PRIVATE | MAP_ANONYMOUS`。

### 私有匿名页的 fork 语义

```text
fork 前已驻留：父子通过 COW 共享，写入后隔离
fork 前未驻留：父子以后分别获得独立零页
```

两种情况都符合私有匿名映射语义。

## 15. `MAP_PRIVATE` 和 `MAP_SHARED`

### 15.1 `MAP_PRIVATE`

- 首次缺页从文件读取或获得匿名零页。
- 当前进程可以修改自己的页。
- fork 时可写驻留页使用 COW。
- `munmap/exit/exec` 不写回文件。

### 15.2 当前 `MAP_SHARED` 的特点

当前 `MAP_SHARED` 是“fork 亲缘进程共享已驻留物理页，并在解除映射时写回”的简化实现。

已经驻留的页在 fork 后：

```text
父 PTE ─┐
        ├──> 同一物理页
子 PTE ─┘
```

父子修改立即互相可见。

但是 fork 时尚未驻留的页会由父子以后分别 `kalloc + readi`，不会指向同一物理页。两个独立 `mmap()` 即使映射同一 inode 和偏移，也会分别分配页面。

原因是当前没有以 `(inode, page offset)` 为键的全局 page cache。

因此当前 `MAP_SHARED` 不具备以下 Linux 语义：

- 独立映射之间的实时一致性。
- fork 后未来缺页仍映射同一个缓存页。
- dirty page 跟踪和后台 writeback。
- `msync()`。

写回发生在：

```text
munmap
exit
成功 exec
```

只要映射是可写 `MAP_SHARED`，所有驻留页都会写回，即使页面没有真正修改。多个独立映射可能用旧页面在较晚的 unmap 中覆盖较新的文件内容，这是当前设计的重要限制。

简历和面试中应称其为“简化文件共享映射”，不要声称实现了 Linux 完整 page cache 一致性。

## 16. `munmap`、exit 和 exec

### 16.1 整段 `munmap`

当前 `sys_munmap(addr, len)`：

- 要求 `addr` 与 VMA 起始地址完全一致。
- 暂时忽略 `len`。
- 解除整个 VMA。
- 跳过尚未驻留的页表空洞。

每个驻留页：

1. 必要时按真实 `len` 限制最后一页写回字节数。
2. `MAP_SHARED` 文件页通过 `filewriteat()` 使用显式 offset 写回。
3. `uvmunmap(..., do_free=1)` 删除 PTE 并递减页引用。
4. 文件 VMA 执行 `fileclose()`，匿名 VMA 跳过。
5. 清空 VMA。

`filewriteat()` 不能修改普通 `file->off`，因为 mmap 的装页/写回不应改变文件描述符当前位置。文件系统写入还要按日志事务允许的大小分块。

### 16.2 exit

mmap 位于 `p->sz` 之外，普通 `proc_freepagetable(pagetable, p->sz)` 不会自动删除高地址叶 PTE。直接进入 `freewalk()` 会因为仍有叶 PTE而 panic。

因此 exit 必须先：

```c
vma_unmap_all_from(p->pagetable, p->vmas, 1);
```

这一步要在仍允许睡眠且尚未持有 `wait_lock/p->lock` 时完成，因为共享映射写回可能进行文件系统 I/O。

### 16.3 exec

exec 失败必须保留旧程序和旧 VMA。只有新页表完全构造成功、准备提交后，才清理旧页表上的 VMA：

```text
构造新页表
全部成功
切换 p->pagetable
清理 oldpagetable 的 VMA
释放 oldpagetable
```

这是一种典型的“先构造、后提交”事务式资源管理。

## 17. 权限与访问语义矩阵

| 页面类型 | fork | 写入行为 | 文件写回 |
| --- | --- | --- | --- |
| 普通可写页 | COW | 复制或独占时恢复 W | 否 |
| 普通只读页 | 只读共享 | 进程被杀死 | 否 |
| lazy heap 未驻留页 | 无 PTE | 首次访问分配私有零页 | 否 |
| 文件 `MAP_PRIVATE + W` | 驻留页 COW | 私有修改 | 否 |
| 文件 `MAP_PRIVATE + R` | 只读共享 | 进程被杀死 | 否 |
| 文件 `MAP_SHARED + W` | 已驻留页直接共享 | 正常写入 | unmap/exit/exec 写回 |
| 文件 `MAP_SHARED + R` | 已驻留页只读共享 | 进程被杀死 | 否 |
| 匿名 `MAP_PRIVATE + W` | 驻留页 COW | 私有修改 | 无文件 |
| 匿名 `MAP_PRIVATE + R` | 只读共享 | 进程被杀死 | 无文件 |

## 18. 资源所有权和失败路径

实现这类功能时，不仅要看成功路径，还要为每一种资源明确所有者。

### 物理页

```text
kalloc 成功后由当前函数持有 ref=1
mappages 成功后引用转移给 PTE
共享给另一个 PTE 前 kaddref
mappages 失败时撤销新增引用
unmap 时 kfree 删除 PTE 引用
```

### 文件引用

```text
sys_mmap 文件映射成功：VMA 持有 filedup 引用
fork：子 VMA 再持有一个 filedup 引用
munmap/exit/exec：每个 VMA 恰好 fileclose 一次
匿名 VMA：file 始终为 0
```

### VMA

VMA 应在所有必需资源准备完成后标记为有效，失败时解除已经创建的 PTE、文件引用和物理页引用。清理函数必须允许 VMA 中存在未驻留页。

## 19. 并发和锁注意事项

### `kmem.lock`

- 保护物理页 freelist 和引用计数。
- 不在持锁时执行可能睡眠的操作。
- `krefcnt()` 返回的是瞬时快照；当前 xv6 没有同一地址空间多线程并发修改 PTE，因此可用于 COW 快路径。

### VMA

当前 xv6 没有共享同一 `struct proc` 地址空间的用户线程，VMA 主要由所属进程串行修改，因此没有独立 VMA 锁。若未来增加线程、并发 `mmap/munmap`，必须引入地址空间锁。

### inode 和 prefault

- 文件 mmap 缺页可能睡眠。
- 不在自旋锁中触发文件装页。
- 尽量在锁外完成 `copyin/copyout` 或提前 `vmfault_range()`。
- 不在持有 VMA/物理页自旋锁时执行文件写回。

### TLB

修改当前正在运行进程的叶 PTE 权限或物理页地址后必须执行 `sfence_vma()`。当前实现使用全局本 hart 刷新，简单但不够精细；多 hart 同一地址空间运行时还需要 TLB shootdown。

## 20. 常见错误清单

1. fork 时给所有只读页设置 `PTE_COW`，导致真正只读页可以被写。
2. 只修改子 PTE，不清除父 PTE 的 `PTE_W`。
3. 修改父 PTE 后忘记 `sfence_vma()`。
4. 共享 PA 前忘记 `kaddref()`。
5. `kfree()` 无视引用计数，提前释放仍被使用的页。
6. `copyout()` 直接写 COW 物理页，绕过硬件 page fault。
7. 只在 `usertrap()` 处理 mmap，忘记 `copyin/copyout/copyinstr`。
8. 把所有 store page fault 都当成 COW。
9. mmap 时立即分配全部物理页，失去惰性装页。
10. 文件短读后未清零页面，泄露旧内存数据。
11. 用 `maplen` 写回最后一页，覆盖用户请求范围之外的数据。
12. mmap 使用普通 `filewrite()`，意外改变 `file->off`。
13. fork 匿名 VMA 时对空 `file` 调用 `filedup()`。
14. unmap 匿名 VMA 时对空 `file` 调用 `fileclose()`。
15. 使用 `flags == MAP_PRIVATE`，漏掉 `MAP_PRIVATE | MAP_ANONYMOUS`。
16. exec 一开始就删除旧 VMA，导致 exec 失败后旧进程映像损坏。
17. 释放页表前没有先解除 `p->sz` 之外的 mmap 叶 PTE。
18. 持有 pipe 自旋锁时触发需要磁盘 I/O 的文件 mmap 缺页。
19. 把当前简化 `MAP_SHARED` 描述成完整的全局共享页缓存。

## 21. 测试与验证

### 构建

```sh
make -j4
make fs.img
```

### COW/mmap 专项测试

进入 xv6 后：

```sh
cowmmaptest
```

覆盖内容包括：

- 普通用户页 fork COW。
- 文件 `MAP_PRIVATE` 内容、写隔离和不写回。
- 文件 `MAP_SHARED` fork 可见性和写回。
- mmap 权限错误。
- mmap 在 exit/exec 中的清理。
- mmap 作为 `read/write` 的用户缓冲区。
- 私有匿名映射零填充。
- 私有匿名映射 fork COW。
- fork 后未驻留匿名页的独立分配。
- 只读匿名页保护。
- 匿名页作为 pipe 的 `copyin/copyout` 缓冲区。
- 拒绝 `MAP_SHARED | MAP_ANONYMOUS`。

预期：

```text
cowmmaptest: OK
```

### 完整回归

```sh
usertests -q
```

预期：

```text
ALL TESTS PASSED
```

测试时还应关注：

- 是否出现 `kfree ref`、`kalloc ref`、`freewalk: leaf` 等 panic。
- fork/exit 循环后是否逐渐耗尽物理内存或文件表。
- 只读写保护测试中的子进程异常是否符合预期，而不是内核 panic。

## 22. 可继续改进的方向

按推荐优先级排序：

1. 实现严格的部分 `munmap`，支持裁剪 VMA 头尾和拆分中间区间。
2. 增加 VMA/地址空间锁，为多线程地址空间做准备。
3. 引入以 inode 和页偏移为键的 page cache，完善 `MAP_SHARED`。
4. 使用 PTE dirty bit 或软件 dirty tracking，避免无修改页面写回。
5. 实现 `msync()` 和更可靠的共享页 writeback。
6. 支持非零页对齐文件 offset。
7. 支持 `PROT_NONE/PROT_EXEC`、`mprotect()`。
8. 支持正确的 `MAP_SHARED | MAP_ANONYMOUS` 共享后端对象。
9. 将 `vmfault_range()` 抽象为统一 uaccess prefault/nofault 接口。
10. 支持多 hart 地址空间共享后的跨核 TLB shootdown。

---

# 23. 简历项目包装：xv6-RISC-V 操作系统功能增强

## 23.1 推荐项目名称

**xv6-RISC-V 内核增强：虚拟内存、PCIe 网卡驱动与 UDP 网络栈**

也可以写成：

**基于 xv6-RISC-V 的 COW/mmap 与 E1000 网络子系统实现**

## 23.2 一句话项目概述

基于 xv6-RISC-V 扩展虚拟内存和网络子系统，实现 fork 写时复制、惰性内存分配、文件/匿名 mmap、PCIe E1000 DMA 网卡驱动、Ethernet/ARP/IPv4/ICMP/UDP 协议栈以及类 POSIX UDP socket API，并在 QEMU 中完成主机与 xv6 的双向 ICMP/UDP 通信验证。

## 23.3 技术关键词

```text
C / RISC-V / Sv39 / Page Table / Page Fault / COW / mmap
Reference Counting / TLB / Lazy Allocation / VMA
PCIe ECAM / BDF / BAR / MMIO / DMA / PLIC / Interrupt
E1000 / Descriptor Ring / mbuf / Ethernet / ARP / IPv4 / ICMP / UDP
Socket / File Descriptor / Spinlock / Sleep-Wakeup / QEMU / TAP / tcpdump
```

## 23.4 系统总体架构

```text
用户程序
├── fork / sbrk / mmap / munmap
│       ↓
│   VMA + vmfault + COW + 引用计数
│       ↓
│   Sv39 页表 / 物理页分配器
│
└── socket / bind / connect / sendto / recvfrom / read / write
        ↓
    UDP socket 层
        ↓
    UDP → IPv4 → ARP → Ethernet
        ↓
    mbuf 所有权传递
        ↓
    E1000 TX/RX DMA 描述符环
        ↓
    PCIe BAR0 MMIO + PLIC IRQ 33
        ↓
    QEMU E1000 → TAP/主机网络
```

## 23.5 可直接放入简历的精简版

**xv6-RISC-V 内核功能增强｜C、RISC-V、QEMU**

- 重构 `fork` 用户内存复制为 COW：使用 RISC-V PTE 软件位标记共享页，为物理页增加引用计数，统一处理 store page fault、TLB 刷新及 `copyout` 绕过硬件写保护的问题。
- 设计 VMA 与统一 `vmfault` 路径，实现 lazy `sbrk`、文件 `MAP_PRIVATE/MAP_SHARED` 和 `MAP_PRIVATE|MAP_ANONYMOUS`；覆盖 fork、munmap、exit、exec、内核 copyin/out 和失败回滚等完整生命周期。
- 基于 PCIe ECAM 枚举 QEMU Intel E1000，探测并配置 BAR0，开启 Memory Space/Bus Master；实现 16 项 TX/RX DMA 描述符环、PLIC 中断、缓冲区补充与所有权回收。
- 实现 mbuf、Ethernet、ARP、IPv4、ICMP Echo 和 UDP 协议路径；ARP 采用 16 项状态缓存及每项最多 8 包待发送队列，支持同网段直达和默认网关下一跳解析。
- 将 UDP socket 集成到 xv6 文件描述符模型，实现 `socket/bind/connect/sendto/recvfrom` 及 `read/write/close/dup/fork` 生命周期，使用自旋锁、睡眠/唤醒和有界接收队列处理并发与资源释放。
- 在 QEMU TAP 环境通过 `ping/tcpdump` 验证 ARP 与 ICMP 4/4 回显，通过 `nettest` 验证主机与 xv6 双向 UDP，并通过 `cowmmaptest` 和完整 `usertests -q` 回归。

## 23.6 详细项目描述

### 虚拟内存增强

- 基于 Sv39 三级页表和 RSW 软件位实现 `PTE_COW`。
- 在物理页分配器中加入页级引用计数，使 `kalloc/kfree/uvmunmap/fork` 共享统一生命周期规则。
- 将普通可写页和 `MAP_PRIVATE` 可写驻留页在 fork 时改为只读 COW，写缺页时根据引用数选择原地恢复权限或分配复制。
- 统一硬件 load/store page fault 与内核 `copyin/copyout/copyinstr` 的访问解析，覆盖 COW、lazy heap、文件 mmap 和匿名 mmap。
- 使用固定 VMA 表和高地址 mmap 区域，实现 first-fit 选址、文件引用持有、惰性装页、共享页写回和 exec 事务式清理。
- 识别锁内用户拷贝触发文件缺页的风险，通过 `vmfault_range` 在 I/O/pipe/wait 路径前预装可能睡眠的映射页。

### PCIe 与 E1000 驱动

- 按 `ECAM_BASE + bus<<20 + device<<15 + function<<12` 计算配置空间地址，扫描 bus 0 的 device/function，匹配 Intel 82540EM `8086:100e`。
- 按 PCI 规范通过 BAR 写全 1 探测 MMIO 空间大小，保留 BAR 属性位并映射到固定 `E1000_MMIO`。
- 设置 PCI Command Register 的 Memory Space 和 Bus Master，使 CPU 可访问寄存器、设备可发起 DMA。
- 初始化 E1000 TX/RX 描述符环、MAC、组播表、发送/接收控制寄存器和中断掩码。
- 明确 mbuf 在协议栈、驱动和硬件之间的所有权；TX 在 DD 回写后回收，RX 在上送前先补充 replacement buffer，内存不足时复用原缓冲避免接收环断裂。
- 使用 `__sync_synchronize()` 保证描述符内容先于 tail 更新对设备可见，并通过 PLIC IRQ 33 完成 RX/TX 中断处理。

### 最小 IPv4/UDP 网络栈

- 使用带 headroom 的 mbuf，发送时依次压入 UDP、IPv4 和 Ethernet 首部，用户 payload 只复制一次。
- 实现网络字节序转换、RFC 1071 IPv4/ICMP 校验和、以太网类型分用和严格长度检查。
- 实现静态 IPv4 `10.0.2.15/24`、默认网关 `10.0.2.2`、同网段/跨网段下一跳选择和 ICMP Echo Reply。
- ARP 表包含 EMPTY、INCOMPLETE、REACHABLE 状态；未解析时暂存 IPv4 mbuf，收到 ARP 后学习 IP-MAC 并在锁外冲刷待发队列。
- ARP 表和 socket 队列均使用有界容量，避免无法解析邻居或用户长期不接收时无限占用内存。

### UDP socket 和文件描述符集成

- 使用 32 项固定 socket 表，每个 socket 最多缓存 16 个 UDP 数据报。
- 支持 `INADDR_ANY`、显式 bind 和 49152～65535 临时端口自动绑定，不支持 `SO_REUSEPORT`。
- UDP `connect()` 只记录默认对端并用于接收过滤，不建立握手状态。
- 接收分用按本地端口、本地地址和可选远端五元组过滤，保持 UDP 数据报边界。
- 队列为空时使用 sleep/wakeup 阻塞，关闭 socket 时使表项失效、唤醒接收者，并在锁外释放 mbuf 队列。
- 引入 `FD_SOCKET`，让 socket 自动继承现有 fd 的 `dup/fork/close/read/write` 引用模型。

## 23.7 最值得讲的四个技术难点

### 难点一：COW 不只处理用户态写缺页

问题：xv6 `copyout()` walk 页表后直接写 PA，不会触发用户 PTE 的硬件写保护。如果只修改 `usertrap()`，内核可能直接改写父子共享页。

解决：让 `copyout()` 每页先调用 `vmfault(..., VM_WRITE)`，断开 COW 后重新 walk 并检查 `PTE_V|PTE_U|PTE_W`。`copyin/copyinstr` 同理处理 lazy/mmap 读缺页。

面试价值：体现对“硬件异常路径”和“内核软件访问路径”差异的理解。

### 难点二：文件 mmap 缺页与锁顺序

问题：mmap 缺页需要 inode 读取并可能睡眠，但 pipe、文件系统或 wait 路径可能在持锁时调用 `copyin/copyout`。

解决：在系统调用进入持锁路径前用 `vmfault_range()` 预装用户缓冲区涉及的映射页，实际 copy 函数仍做最终权限验证。

面试价值：可以讨论自旋锁不能睡眠、prefault、锁顺序和 Linux uaccess 的设计差异。

### 难点三：E1000 DMA 描述符和 mbuf 所有权

问题：CPU 和设备异步访问描述符与数据缓冲，如果过早释放 TX mbuf 或 RX 环没有及时补充缓冲，会产生 use-after-free 或接收停顿。

解决：以 DD/EOP 状态位划分软硬件所有权，TX 完成后回收；RX 上送前先安装 replacement mbuf；tail 更新前使用内存屏障。

面试价值：可以讲 DMA、一致性、内存屏障、环形队列和中断上下文。

### 难点四：ARP 未命中时的数据包生命周期

问题：IPv4 发送时可能还不知道下一跳 MAC，不能丢失原 UDP/IP 包，也不能无限缓存。

解决：ARP 状态机把表项置为 INCOMPLETE，将完整 IPv4 mbuf 加入有界 pending 队列，发送广播请求；学习映射后在锁内摘链、锁外逐包发送。

面试价值：体现状态机、资源上限、锁粒度和跨层所有权设计。

## 23.8 常见面试追问与回答要点

### 为什么 COW 要清除父进程的写权限？

如果只把子页设为只读，父进程仍能直接写共享页，子进程看到的快照会被破坏。父子都必须只读并设置 COW。

### 为什么只读代码页不设置 COW？

COW 表示“原本允许写，只是暂时共享”。代码页本来就不可写，写入应当是保护错误。

### 引用计数为 1 时为什么不用复制？

此时已经没有其他映射观察旧页，直接清除 COW 并恢复写权限即可，避免无意义的分配和 4KB 拷贝。

### 为什么 mmap 需要 `len` 和 `maplen`？

页表操作以完整页为单位，文件读写以用户请求字节数为边界。混用会导致最后一页越界写回。

### 当前 `MAP_SHARED` 为什么不完全正确？

没有全局 page cache。只有 fork 时已经驻留的页直接共享；未来缺页和独立 mmap 会分配不同物理页，写回也没有 dirty tracking。

### 为什么匿名私有映射的 fd 必须是 -1？

匿名 VMA 没有文件后端。要求 -1 可以明确 ABI 语义，也能尽早发现错误调用。

### ECAM 和 BAR 有什么区别？

ECAM 映射 PCIe 配置空间，用来发现设备和配置资源；BAR 描述设备运行寄存器/显存等窗口，配置后驱动通过 BAR 对应 MMIO 操作设备。

### 为什么 PCIe 要开启 Bus Master？

E1000 需要主动从内存读取 TX 描述符和数据，并把 RX 数据写入内存；没有 Bus Master 权限，设备不能发起 DMA。

### 为什么发送路径要在更新 TDT 前加内存屏障？

必须保证描述符地址、长度和命令已经对设备可见，设备看到新的 tail 后才能读取完整描述符。

### 为什么不能持有 ARP 锁调用 `eth_tx()`？

下层发送可能获取 E1000 锁或触发复杂路径。锁内只更新状态、摘取队列，锁外发送可以缩短临界区并避免锁顺序问题。

### UDP connect 为什么不需要三次握手？

UDP 无连接。这里的 connect 只保存默认目的 IP/端口，并让接收路径过滤非目标对端的数据报。

### 为什么 socket 接收循环反复检查 `s->used`？

阻塞接收期间另一个引用可能关闭最后一个 fd。close 会清空 socket 并唤醒等待者，醒来后必须识别对象已失效，避免访问被复用的表项。

## 23.9 项目边界要诚实说明

面试中主动说明边界通常比夸大功能更有说服力：

- mmap 只支持内核选址、offset 0 和整段 munmap。
- `MAP_SHARED` 是简化版，没有全局 page cache 和 dirty tracking。
- 只实现私有匿名映射，没有共享匿名后端。
- 网络层使用静态 IPv4，不支持 DHCP、IPv4 分片重组和 TCP。
- UDP 第一版不实现完整 checksum、错误队列和 socket options。
- PCIe 只扫描 bus 0，不递归枚举 bridge，不支持 64 位 BAR、MSI/MSI-X 和通用资源分配。
- E1000 使用固定大小描述符环，没有 NAPI、零拷贝用户 I/O 和高负载拥塞控制。

这些限制是有意识的范围控制：先建立从系统调用、协议栈、驱动到硬件模型的完整闭环，再逐步扩展复杂语义。

## 23.10 30 秒面试介绍

> 我基于 xv6-RISC-V 做了两个方向的内核增强。虚拟内存方面，我把 fork 改成 COW，为物理页加入引用计数，并设计统一 vmfault 路径处理 COW、lazy sbrk、文件 mmap 和私有匿名 mmap，同时补齐 copyin/out、fork、munmap、exit、exec 的生命周期。网络方面，我从 PCIe ECAM 枚举和 E1000 BAR/DMA 描述符环开始，向上实现 Ethernet、ARP、IPv4、ICMP、UDP 和类 POSIX socket API，最终在 QEMU TAP 中完成主机到 xv6 的 ping 和双向 UDP 测试。这个项目让我把页表、异常、锁、DMA、中断和协议状态机串成了一个完整系统。

## 24. 核心总结

COW/mmap 最核心的原则是：

> VMA 决定地址和访问是否合法，PTE_COW 决定一个有效只读页能否通过复制变成可写；硬件 page fault 和内核 copyin/out 都必须遵守同一套页面状态与资源所有权规则。

整个项目最核心的工程原则是：

> 为每一层明确状态、所有权、锁和失败回滚：虚拟页由 VMA/PTE 描述，物理页由引用计数管理，mbuf 在协议栈与 DMA 环之间转移，socket/file/fd 使用分层引用生命周期；跨层调用前先释放不应长期持有的锁。
