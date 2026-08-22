#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "vm.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

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
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy its low user address space into a
// child's page table using copy-on-write mappings.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint64 flags;
  int changed = 0;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if (flags & PTE_W) {
      flags = (flags & ~PTE_W) | PTE_COW;
      *pte = PA2PTE(pa) | flags;
      changed = 1;
    }
    kaddref(pa);
    if (mappages(new, i, PGSIZE, pa, flags) != 0) {
      kfree((void *)pa);
      goto err;
    }
  }
  if (changed)
    sfence_vma();
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  if (changed)
    sfence_vma();
  return -1;
}

// Make a COW mapping writable, copying its page if another mapping still
// refers to the old physical page.
int
cow_break(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 oldpa, flags;
  char *mem;

  va = PGROUNDDOWN(va);
  pte = walk(pagetable, va, 0);
  if (pte == 0 || (*pte & (PTE_V | PTE_COW)) != (PTE_V | PTE_COW))
    return -1;

  oldpa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);
  if (krefcnt(oldpa) == 1) {
    flags = (flags | PTE_W) & ~PTE_COW;
    *pte = PA2PTE(oldpa) | flags;
    sfence_vma();
    return 0;
  }

  mem = kalloc();
  if (mem == 0)
    return -1;
  memmove(mem, (void *)oldpa, PGSIZE);
  flags = (flags | PTE_W) & ~PTE_COW;
  *pte = PA2PTE((uint64)mem) | flags;
  kfree((void *)oldpa);
  sfence_vma();
  return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;
  struct proc *p = myproc();

  (void)psz;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    if (p != 0 && pagetable == p->pagetable &&
        vmfault(p, dstva, VM_WRITE) < 0)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & (PTE_V | PTE_U | PTE_W)) !=
                        (PTE_V | PTE_U | PTE_W))
      return -1;
    pa0 = PTE2PA(*pte);

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;
  struct proc *p = myproc();

  (void)psz;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    if (va0 >= MAXVA)
      return -1;
    if (p != 0 && pagetable == p->pagetable &&
        vmfault(p, srcva, VM_READ) < 0)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & (PTE_V | PTE_U | PTE_R)) !=
                        (PTE_V | PTE_U | PTE_R))
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;
  pte_t *pte;
  struct proc *p = myproc();

  (void)psz;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    if (va0 >= MAXVA)
      return -1;
    if (p != 0 && pagetable == p->pagetable &&
        vmfault(p, srcva, VM_READ) < 0)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & (PTE_V | PTE_U | PTE_R)) !=
                        (PTE_V | PTE_U | PTE_R))
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

static struct vma *
vma_find(struct proc *p, uint64 va)
{
  for (int i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    if (v->used && va >= v->addr && va - v->addr < v->maplen)
      return v;
  }
  return 0;
}

static int
mmap_fault_page(struct proc *p, struct vma *v, uint64 va)
{
  char *mem;
  uint64 pageoff, fileoff, n;
  int r = 0, perm = PTE_U;
  int already_locked;

  mem = kalloc();
  if (mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  pageoff = va - v->addr;
  if (pageoff >= v->len) {
    kfree(mem);
    return -1;
  }
  n = PGSIZE;
  if (n > v->len - pageoff)
    n = v->len - pageoff;
  fileoff = v->offset + pageoff;

  if (fileoff <= 0xffffffffU) {
    already_locked = holdingsleep(&v->file->ip->lock);
    if (!already_locked)
      ilock(v->file->ip);
    r = readi(v->file->ip, 0, (uint64)mem, (uint)fileoff, (uint)n);
    if (!already_locked)
      iunlock(v->file->ip);
  }
  if (r < 0) {
    kfree(mem);
    return -1;
  }

  if (v->prot & PROT_READ)
    perm |= PTE_R;
  if (v->prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0) {
    kfree(mem);
    return -1;
  }
  return 0;
}

// Resolve a read or write to the current process's address space. This handles
// existing COW pages, lazy heap pages, and lazy file-backed pages.
int
vmfault(struct proc *p, uint64 va, int access)
{
  pte_t *pte;
  uint64 va0;
  char *mem;
  struct vma *v;

  if (p == 0 || va >= MAXVA || (access != VM_READ && access != VM_WRITE))
    return -1;
  va0 = PGROUNDDOWN(va);
  pte = walk(p->pagetable, va0, 0);
  if (pte != 0 && (*pte & PTE_V)) {
    if ((*pte & PTE_U) == 0)
      return -1;
    if (access == VM_WRITE && (*pte & PTE_COW))
      return cow_break(p->pagetable, va0);
    if (access == VM_WRITE && (*pte & PTE_W) == 0)
      return -1;
    if (access == VM_READ && (*pte & PTE_R) == 0)
      return -1;
    return 0;
  }

  if (va < p->sz && va < MMAPBASE) {
    mem = kalloc();
    if (mem == 0)
      return -1;
    memset(mem, 0, PGSIZE);
    if (mappages(p->pagetable, va0, PGSIZE, (uint64)mem,
                 PTE_R | PTE_W | PTE_U) < 0) {
      kfree(mem);
      return -1;
    }
    return 0;
  }

  v = vma_find(p, va);
  if (v == 0)
    return -1;
  if (access == VM_READ && (v->prot & PROT_READ) == 0)
    return -1;
  if (access == VM_WRITE && (v->prot & PROT_WRITE) == 0)
    return -1;
  return mmap_fault_page(p, v, va0);
}

int
vmfault_range(struct proc *p, uint64 va, uint64 len, int access)
{
  uint64 a, last;

  if (len == 0)
    return 0;
  if (va >= MAXVA || len > MAXVA - va)
    return 0;
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + len - 1);
  for (;;) {
    pte_t *pte = walk(p->pagetable, a, 0);
    if ((pte == 0 || (*pte & PTE_V) == 0)) {
      struct vma *v = vma_find(p, a);
      if (v != 0 &&
          ((access == VM_READ && (v->prot & PROT_READ)) ||
           (access == VM_WRITE && (v->prot & PROT_WRITE))) &&
          vmfault(p, a, access) < 0)
        return -1;
    }
    if (a == last)
      break;
    a += PGSIZE;
  }
  return 0;
}

// Copy VMA metadata and already-resident mmap pages during fork. Pages in
// private writable mappings become COW; shared pages remain directly shared.
int
vma_fork(struct proc *parent, struct proc *child)
{
  int changed = 0;

  for (int i = 0; i < NVMA; i++) {
    struct vma *src = &parent->vmas[i];
    struct vma *dst = &child->vmas[i];
    if (!src->used)
      continue;

    *dst = *src;
    dst->file = filedup(src->file);
    for (uint64 a = src->addr; a < src->addr + src->maplen; a += PGSIZE) {
      pte_t *pte = walk(parent->pagetable, a, 0);
      uint64 pa, flags;

      if (pte == 0 || (*pte & PTE_V) == 0)
        continue;
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if (src->flags == MAP_PRIVATE && (flags & PTE_W)) {
        flags = (flags & ~PTE_W) | PTE_COW;
        *pte = PA2PTE(pa) | flags;
        changed = 1;
      }
      kaddref(pa);
      if (mappages(child->pagetable, a, PGSIZE, pa, flags) < 0) {
        kfree((void *)pa);
        goto bad;
      }
    }
  }
  if (changed)
    sfence_vma();
  return 0;

bad:
  if (changed)
    sfence_vma();
  vma_unmap_all_from(child->pagetable, child->vmas, 0);
  return -1;
}

static int
vma_unmap_from(pagetable_t pagetable, struct vma *v, int writeback)
{
  int ret = 0;

  if (!v->used)
    return -1;
  for (uint64 a = v->addr; a < v->addr + v->maplen; a += PGSIZE) {
    pte_t *pte = walk(pagetable, a, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      continue;

    if (writeback && v->flags == MAP_SHARED && (v->prot & PROT_WRITE)) {
      uint64 pageoff = a - v->addr;
      uint64 n = PGSIZE;
      if (n > v->len - pageoff)
        n = v->len - pageoff;
      if (filewriteat(v->file, PTE2PA(*pte), v->offset + pageoff, (int)n) < 0)
        ret = -1;
    }
    uvmunmap(pagetable, a, 1, 1);
  }
  fileclose(v->file);
  memset(v, 0, sizeof(*v));
  return ret;
}

int
vma_unmap(struct proc *p, struct vma *v, int writeback)
{
  return vma_unmap_from(p->pagetable, v, writeback);
}

void
vma_unmap_all_from(pagetable_t pagetable, struct vma *vmas, int writeback)
{
  for (int i = 0; i < NVMA; i++)
    if (vmas[i].used)
      vma_unmap_from(pagetable, &vmas[i], writeback);
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}
