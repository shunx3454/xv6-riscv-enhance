// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  int refcnt[(PHYSTOP - KERNBASE) / PGSIZE];  // kernel base 到 的PHYSTOP 物理页引用计数
} kmem;

// 从kernel base 的 物理页索引
#define PAINDEX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

// 仅初始化调用
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    kmem.refcnt[PAINDEX(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  if (kmem.refcnt[PAINDEX(pa)] < 1)
    panic("kfree ref");
  if (--kmem.refcnt[PAINDEX(pa)] > 0) {
    release(&kmem.lock);
    return;
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    if (kmem.refcnt[PAINDEX(r)] != 0)
      panic("kalloc ref");
    kmem.refcnt[PAINDEX(r)] = 1;
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Add a reference to an allocated physical page.
void
kaddref(uint64 pa)
{
  if ((pa % PGSIZE) != 0 || pa < (uint64)end || pa >= PHYSTOP)
    panic("kaddref");

  acquire(&kmem.lock);
  if (kmem.refcnt[PAINDEX(pa)] < 1)
    panic("kaddref ref");
  kmem.refcnt[PAINDEX(pa)]++;
  release(&kmem.lock);
}

// Return the current reference count of an allocated physical page.
int
krefcnt(uint64 pa)
{
  int n;

  if ((pa % PGSIZE) != 0 || pa < (uint64)end || pa >= PHYSTOP)
    panic("krefcnt");

  acquire(&kmem.lock);
  n = kmem.refcnt[PAINDEX(pa)];
  release(&kmem.lock);
  return n;
}
