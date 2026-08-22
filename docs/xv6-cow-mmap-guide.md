总体上，建议把 COW 和 mmap 统一成一个“用户页可访问性解析器”，但保留两个不同的判定依据：

* VMA 描述“这个虚拟地址是否合法、对应哪个文件、允许什么访问”。
* PTE_COW 描述“这个已经存在的只读 PTE，实际上是否允许通过复制变成可写”。

这样可以避免把 mmap 只读页误判成 COW 页，也能让 `copyin/copyout` 与用户态缺页走同一套逻辑。

以下按：

```c
#define MMAPBASE (MAXVA / 2)
```

讨论；如果你的代码里已经把这个边界定义成 `MAXVMA/2`，直接替换即可。

## 1. 地址空间布局

```text
0
│ ELF text/data
│ heap
│ p->sz
│
├──────────────────────── MMAPBASE = MAXVA / 2
│ mmap VMA 0
│ 空洞
│ mmap VMA 1
│ ...
├──────────────────────── TRAPFRAME
│ trapframe
├──────────────────────── TRAMPOLINE
```

必须保证：

```c
vma->addr >= MMAPBASE
vma->addr + vma->maplen <= TRAPFRAME
```

同时限制 `sbrk/exec` 生成的普通地址空间不能超过 `MMAPBASE`，从而让低地址普通内存和高地址 mmap 永不碰撞。

## 2. 数据结构

```c
#define NVMA 8

struct vma {
  int used;

  uint64 addr;       // 页对齐后的起始地址
  uint64 len;        // 用户请求的原始长度
  uint64 maplen;     // PGROUNDUP(len)

  int prot;          // PROT_READ / PROT_WRITE
  int flags;         // MAP_PRIVATE / MAP_SHARED

  struct file *file; // 独立持有一次 file 引用
  uint64 offset;     // 当前恒为 0，但仍建议保留
};

struct proc {
  // ...
  struct vma vmas[NVMA];
};
```

区分 `len` 和 `maplen` 很重要：

* 建立和解除页表映射时使用 `maplen`。
* 读文件、写回最后一页时使用 `len` 限制字节数，避免无条件写回页尾的无效部分。

COW 使用 RISC-V PTE 的 RSW 软件位：

```c
#define PTE_COW (1L << 8)
```

不必增加 `PTE_MMAP`。某地址是不是 mmap 地址应由 VMA 判断，避免 VMA 和 PTE 中产生两份状态。

## 3. mmap 系统调用

建议验证：

```c
addr == 0
offset == 0
len > 0
prot 至少包含 PROT_READ 或 PROT_WRITE
flags 恰好为 MAP_PRIVATE 或 MAP_SHARED
fd 有效
file->type == FD_INODE
file->readable
```

如果是：

```c
MAP_SHARED && (prot & PROT_WRITE)
```

还应该要求：

```c
file->writable
```

而 `MAP_PRIVATE | PROT_WRITE` 可以基于只读 fd，因为修改不会写回文件。

### 地址分配

固定只有 8 个 VMA，可以直接在 `[MMAPBASE, TRAPFRAME)` 中做 O(8²) first-fit：

1. 从 `MMAPBASE` 开始。
2. 检查候选区间是否和已有 VMA 重叠。
3. 重叠就移动到该 VMA 结尾。
4. 找到足够大的空洞后返回。
5. 没有空闲 VMA 槽或地址区间不足则失败。

### mmap 本身不分配物理页

成功时只做：

```c
v->used = 1;
v->addr = chosen_addr;
v->len = len;
v->maplen = PGROUNDUP(len);
v->prot = prot & (PROT_READ | PROT_WRITE);
v->flags = flags;
v->file = filedup(f);
v->offset = 0;
```

不能调用 `kalloc()`，也不能建立 PTE。官方实验同样要求 mmap 采用惰性装页，使映射大文件不会立即消耗大量物理内存。[MIT xv6 mmap lab](https://pdos.csail.mit.edu/6.1810/2025/labs/mmap.html)

### PROT_WRITE 的 RISC-V 陷阱

RISC-V 不支持合法的 `W=1,R=0` 叶 PTE；这种编码是保留编码。因此简化实现通常令：

```c
if(prot & PROT_WRITE)
  perm |= PTE_R | PTE_W;
else if(prot & PROT_READ)
  perm |= PTE_R;
```

也就是说，用户传入 `PROT_WRITE` 时，硬件 PTE 实际会变成可读写。如果必须严格实现 write-only，只能拒绝 `PROT_WRITE` 但没有 `PROT_READ` 的映射。

忽略 `PROT_EXEC` 时不要设置 `PTE_X`，指令页错误直接杀死进程。

## 4. 统一缺页处理

建议实现：

```c
#define VM_READ  1
#define VM_WRITE 2

int vmfault(struct proc *p, uint64 va, int access);
```

其核心判定顺序应当是：

```c
va0 = PGROUNDDOWN(va);
pte = walk(p->pagetable, va0, 0);

if(pte 存在且 PTE_V) {
  if(access == VM_WRITE && (*pte & PTE_COW))
    return cow_break(p->pagetable, va0);

  if(access == VM_WRITE && !(*pte & PTE_W))
    return -1;

  if(access == VM_READ && !(*pte & PTE_R))
    return -1;

  return 0;
}

// PTE 不存在：检查是否属于惰性 mmap
v = vma_find(p, va);

if(v == 0)
  return -1;

if(access == VM_READ && !(v->prot & PROT_READ))
  return -1;

if(access == VM_WRITE && !(v->prot & PROT_WRITE))
  return -1;

return mmap_fault_page(p, v, va0);
```

关键点是：

* `有效 + PTE_COW`：COW 写错误。
* `有效 + 非 COW + 不可写`：真正的保护错误。
* `无有效 PTE + 地址属于 VMA`：mmap 惰性装页。
* `无有效 PTE + 不属于 VMA`：非法地址。

不能把所有 store page fault 都当成 COW。

### mmap_fault_page

```c
int
mmap_fault_page(struct proc *p, struct vma *v, uint64 va0)
{
  char *mem = kalloc();
  if(mem == 0)
    return -1;

  memset(mem, 0, PGSIZE);

  uint64 pageoff = va0 - v->addr;
  uint64 n = PGSIZE;

  if(pageoff + n > v->len)
    n = v->len - pageoff;

  ilock(v->file->ip);
  int r = readi(v->file->ip, 0, (uint64)mem,
                v->offset + pageoff, n);
  iunlock(v->file->ip);

  if(r < 0) {
    kfree(mem);
    return -1;
  }

  int perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;

  if(mappages(p->pagetable, va0, PGSIZE,
              (uint64)mem, perm) < 0) {
    kfree(mem);
    return -1;
  }

  return 0;
}
```

必须先清零页面，因为：

* 文件可能比映射短。
* `readi()` 可能只读取部分数据。
* 页尾需要保持为零。

在 `usertrap()` 中：

```c
if(r_scause() == 13) {          // load page fault
  if(vmfault(p, r_stval(), VM_READ) < 0)
    setkilled(p);
} else if(r_scause() == 15) {   // store page fault
  if(vmfault(p, r_stval(), VM_WRITE) < 0)
    setkilled(p);
}
```

`scause == 12` 是 instruction page fault；因为没有实现执行权限，直接杀死进程。

## 5. COW 物理页引用计数

所有可共享的用户物理页都必须使用统一引用计数：

```c
refcnt[(pa - KERNBASE) / PGSIZE]
```

语义：

* `kalloc()` 返回页面时引用数为 1。
* fork 将 PA 映射到子进程前执行 `kaddref(pa)`。
* `kfree(pa)` 先递减引用数。
* 只有引用数变成 0 才真正加入空闲链表。

`uvmunmap(..., do_free=1)` 最终调用的 `kfree()` 也必须是引用计数版本，否则解除一个共享映射会提前释放仍被其他进程使用的页面。

### COW 写时复制

```c
int
cow_break(pagetable_t pt, uint64 va0)
{
  pte_t *pte = walk(pt, va0, 0);

  if(pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_COW))
    return -1;

  uint64 oldpa = PTE2PA(*pte);
  uint64 flags = PTE_FLAGS(*pte);

  if(krefcnt(oldpa) == 1) {
    flags = (flags | PTE_W) & ~PTE_COW;
    *pte = PA2PTE(oldpa) | flags;
    sfence_vma();
    return 0;
  }

  char *mem = kalloc();
  if(mem == 0)
    return -1;

  memmove(mem, (void *)oldpa, PGSIZE);

  flags = (flags | PTE_W) & ~PTE_COW;
  *pte = PA2PTE((uint64)mem) | flags;

  kfree((void *)oldpa);   // 减少旧页引用
  sfence_vma();
  return 0;
}
```

只有原本可写的页才能在 fork 时设置 `PTE_COW`。原本只读的代码页、只读 mmap 页直接共享只读 PTE，不能设置 COW，否则写只读页会被错误地允许。

## 6. fork：普通页和 mmap 页必须分别处理

普通地址空间仍遍历 `[0, p->sz)`。

### 普通页

| 父页状态    | fork 后处理                      |
| ------- | ----------------------------- |
| 可写页     | 父子清 `PTE_W`、置 `PTE_COW`、共享 PA |
| 已经是 COW | 子进程继续映射同一 PA，并增加引用            |
| 只读页     | 原样共享，不设置 COW                  |

修改父进程 PTE 后必须刷新 TLB。

### VMA 元数据

每个有效 VMA：

```c
child->vmas[i] = parent->vmas[i];
child->vmas[i].file = filedup(parent->vmas[i].file);
```

只复制 VMA 元数据还不完全够：如果父进程在 fork 前已经修改过 `MAP_PRIVATE` 页面，而子进程重新从文件装页，子进程会丢失 fork 时的内存快照。

因此推荐同时遍历父 VMA 中已经驻留的页面：

| VMA/PTE 类型        | fork 处理              |
| ----------------- | -------------------- |
| 未驻留页面             | 子进程保持无 PTE，之后惰性装页    |
| `MAP_PRIVATE` 可写页 | 父子共享 PA，清 W、设置 COW   |
| `MAP_PRIVATE` 只读页 | 共享只读 PA              |
| `MAP_SHARED` 驻留页  | 共享同一 PA，保留 W，不设置 COW |

这样：

* private 映射具有正确的 fork 快照和写时隔离。
* 已驻留的 shared 页在父子之间真正可见。
* mmap 页的物理引用仍由同一套引用计数管理。

官方实验允许父子分别为 `MAP_SHARED` 分配物理页，因此“只复制 VMA、子进程重新装页”也可能通过基础测试；但那会弱化 fork 快照和共享可见性。若要求所有独立 mmap、所有未来缺页都严格共享，需要引入以 `(inode, file-page-offset)` 为键的共享页缓存，这已经超出当前简化实现。[MIT xv6 mmap lab](https://pdos.csail.mit.edu/6.1810/2025/labs/mmap.html)

### fork 失败回滚

子进程构造失败时必须：

1. 解除已经建立的 mmap PTE，递减物理页引用。
2. `fileclose()` 已复制的 VMA 文件引用。
3. 清空 VMA。
4. 再调用普通的 `freeproc()`。

不需要把父进程已经改成 COW 的 PTE 恢复成可写；父进程以后写入时发现引用数为 1，可以直接恢复写权限。

## 7. copyin、copyout、copyinstr

这是 COW 和 mmap 合并时最容易遗漏的地方。

用户态访问会触发硬件 page fault，但内核的：

```c
copyout()
copyin()
copyinstr()
```

通过页表找到 PA 后直接 `memmove()`，不会触发用户态页错误。

因此：

### copyout

对每个目标页面先执行：

```c
vmfault(p, dstva, VM_WRITE)
```

它会：

* 为尚未装入的可写 mmap 页读入文件页面。
* 对 COW 页执行复制。
* 拒绝只读 mmap 页。
* 拒绝非法地址。

然后重新 walk PTE，并确认：

```c
PTE_V | PTE_U | PTE_W
```

最后才能 `memmove()`。

不能只调用原来的 `walkaddr()`，因为它通常只检查 `PTE_V/PTE_U`，不会正确处理 `PTE_W` 和 `PTE_COW`。

### copyin/copyinstr

每进入一个新页面，先执行：

```c
vmfault(p, srcva, VM_READ)
```

这样下面的操作才能正常工作：

```c
write(fd, mmap_addr, n);   // 内核从尚未装页的 mmap 区 copyin
exec(mmap_addr, argv);     // copyinstr 读取 mmap 字符串
```

读 COW 页不需要复制，直接读取共享旧页即可。

### API 兼容方案

当前 xv6 的 `copyin/out` 只有 `pagetable_t` 参数。[官方 vm.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c)

改动较小的方案是在里面判断：

```c
struct proc *p = myproc();

if(p != 0 && pagetable == p->pagetable)
  vmfault(p, va, access);
```

这样 syscall 对当前进程地址空间的复制会处理 COW/mmap，而 `exec()` 构造临时页表时：

```c
pagetable != p->pagetable
```

仍使用原始复制逻辑。

更干净但改动更大的方案是增加 `copyout_proc()`、`copyin_proc()`、`copyinstr_proc()`，显式接收 `struct proc *`。

## 8. munmap 和写回

因为只支持整段解除，建议要求：

```c
addr == vma->addr
```

`length` 参数读取但忽略。不要允许传入 VMA 中间地址，否则“忽略 length”会产生非常意外的行为。

处理流程：

1. 找到起始地址完全匹配的 VMA。
2. 遍历 `[vma->addr, vma->addr + vma->maplen)`。
3. 跳过没有有效 PTE 的惰性空洞。
4. 对需要写回的驻留页进行写回。
5. 解除 PTE，递减物理页引用。
6. `fileclose(vma->file)`。
7. 清空 VMA。

写回条件建议为：

```c
(v->flags == MAP_SHARED) &&
(v->prot & PROT_WRITE)
```

基础实现可以写回所有驻留页面，不检查 dirty bit；官方测试明确允许这样做。[MIT xv6 mmap lab](https://pdos.csail.mit.edu/6.1810/2025/labs/mmap.html)

写回大小：

```c
pageoff = va - v->addr;
n = min(PGSIZE, v->len - pageoff);
fileoff = v->offset + pageoff;
```

### 不要直接调用 filewrite

`filewrite()` 使用并修改 `file->off`，但 mmap 缺页和写回不应该影响普通文件描述符的当前位置。

应实现类似：

```c
filewriteat(struct file *f, uint64 src,
            uint64 fileoff, int n);
```

内部使用显式 offset 的 `writei()`。

此外，一个 4096 字节页可能超过一次日志事务允许修改的数据量，应参考 `filewrite()` 分块：

```c
begin_op();
ilock(ip);
writei(ip, 0, src, off, chunk);
iunlock(ip);
end_op();
```

不要在持有 `p->lock` 或引用计数自旋锁时执行文件系统 I/O。

### 稀疏解除

原版 `uvmunmap()` 遇到不存在的 PTE 可能 panic，而 mmap 是惰性的，VMA 中有未装页空洞是正常情况。

可以新增：

```c
uvmunmap_sparse(pagetable, va, npages, do_free);
```

对无 PTE、无 `PTE_V` 的页面直接跳过；不要简单修改所有调用者依赖的原版严格语义。

## 9. exit、exec 和页表销毁

mmap 区域位于 `p->sz` 之外，普通的：

```c
proc_freepagetable(pagetable, p->sz)
```

不会自动解除这些高地址叶 PTE。若直接进入 `freewalk()`，很可能因为仍存在叶 PTE 而 panic。

### exit

在释放页表之前调用：

```c
vma_unmap_all(p, 1);  // 1 表示 MAP_SHARED 写回
```

每个 VMA执行与整段 `munmap` 相同的逻辑，并关闭独立的文件引用。

应在仍允许睡眠、且未持有 `wait_lock/p->lock` 时做文件写回。

### exec

`exec` 失败时必须保留旧 VMA；只有新程序页表完全创建成功后才能清理旧映射。

推荐让清理函数显式接收页表：

```c
vma_unmap_all_from(oldpagetable, p->vmas, 1);
proc_freepagetable(oldpagetable, oldsz);
memset(p->vmas, 0, sizeof(p->vmas));
```

否则 `exec` 成功后旧 mmap 页和文件引用都会泄漏。

## 10. 最终访问语义矩阵

| 页面类型              | fork        | 写错误处理      | munmap/exit 写回 |
| ----------------- | ----------- | ---------- | -------------- |
| 普通可写页             | COW 共享      | 复制或独占时恢复 W | 否              |
| 普通只读页             | 只读共享        | 杀死进程       | 否              |
| `MAP_PRIVATE + W` | COW 共享      | 复制         | 否              |
| `MAP_PRIVATE + R` | 只读共享        | 杀死进程       | 否              |
| `MAP_SHARED + W`  | 建议直接共享可写 PA | 正常写入       | 是              |
| `MAP_SHARED + R`  | 只读共享        | 杀死进程       | 不需要            |
| 尚未装页的 VMA         | 保持无 PTE     | 从文件惰性装页    | 不写回            |

## 11. 推荐修改位置

* `kernel/riscv.h`

  * `PTE_COW`，可选 `PTE_D`
* `kernel/kalloc.c`

  * 物理页引用计数、`kaddref/krefcnt`
* `kernel/proc.h`

  * `struct vma`、`vmas[8]`
* `kernel/proc.c`

  * VMA 初始化、fork 继承、exit 清理、失败回滚
* `kernel/vm.c`

  * COW fork、`cow_break`、访问感知版 copyin/out、稀疏 unmap
* `kernel/trap.c`

  * load/store page fault 调用 `vmfault`
* `kernel/sysfile.c`

  * `sys_mmap/sys_munmap`
* `kernel/file.c`

  * 可选 `filewriteat`
* `kernel/syscall.h`、`syscall.c`

  * syscall 编号和分发表
* `user/user.h`、`user/usys.pl`

  * 用户接口
* `kernel/fcntl.h`

  * `PROT_READ/WRITE`、`MAP_PRIVATE/SHARED`

当前官方基线中的相关入口可以对照 [vm.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c)、[trap.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c) 和 [proc.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c)。

最核心的实现原则可以压缩成一句话：

> VMA 判断“地址和访问是否合法”，PTE_COW 判断“只读 PTE 是否可以通过复制变成可写”，所有用户内存访问路径——硬件缺页和内核 copyin/out——都必须经过这两层判断。
