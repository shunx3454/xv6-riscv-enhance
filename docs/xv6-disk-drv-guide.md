# xv6 Disk VirtIO Driver Guide

本文聚焦 xv6 中 `virtio_disk_rw()` 如何配合 VirtIO 的共享数据结构完成一次磁盘请求的组织、提交与回收。

本文假设已经了解：

- 中断基本机制
- `sleep()` / `wakeup()`
- 进程调度
- buffer cache 的基本作用

因此这里重点关注：

1. `disk.desc`
2. `disk.avail`
3. `disk.used`
4. `disk.free`
5. `disk.info`
6. `disk.ops`
7. `disk.used_idx`
8. `io_fence()`

---

# 1. `struct disk` 的整体作用

xv6 的 VirtIO block driver 使用下面这些结构管理磁盘请求：

```c
static struct disk {
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;

  char free[NUM];
  uint16 used_idx;

  struct {
    struct buf *b;
    char status;
  } info[NUM];

  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;
} disk;
```

可以把它们分成三类。

## 1.1 VirtIO 与设备共享的数据

```c
disk.desc
disk.avail
disk.used
```

这些数据结构属于 VirtIO 协议的一部分。

驱动与设备通过它们交换：

- 请求描述
- 待处理请求
- 已完成请求

---

## 1.2 一次请求中会被设备 DMA 访问的数据

```c
disk.ops[i]
b->data
disk.info[i].status
```

descriptor 的 `addr` 字段会指向这些内存区域。

---

## 1.3 xv6 驱动自己的 bookkeeping

```c
disk.free
disk.used_idx
disk.info[i].b
disk.vdisk_lock
```

这些内容设备并不理解，只是 xv6 自己用来管理 descriptor 和请求生命周期。

---

# 2. `desc[]` 是 descriptor 池，不是 ring

```c
struct virtq_desc *desc;
```

假设：

```c
#define NUM 8
```

逻辑上就是：

```text
desc[0]
desc[1]
desc[2]
desc[3]
desc[4]
desc[5]
desc[6]
desc[7]
```

它们是 8 个独立 descriptor。

descriptor 大致包含：

```c
struct virtq_desc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};
```

含义：

```text
addr   描述哪一段内存
len    内存长度
flags  访问方向、是否存在 next
next   下一个 descriptor 的编号
```

descriptor 本身并不天然相连。

一次请求如果需要多个 descriptor，就通过：

```c
flags |= VRING_DESC_F_NEXT;
next = ...
```

把它们串成链表。

---

# 3. `free[]` 管理 descriptor 是否空闲

```c
char free[NUM];
```

它与 `desc[]` 一一对应：

```text
free[i] ↔ desc[i]
```

例如：

```text
index      0 1 2 3 4 5 6 7
free[]     1 0 0 1 1 0 1 1
```

表示：

```text
desc[1]
desc[2]
desc[5]
```

当前已经被某个请求占用。

因此：

```text
free[] 管 descriptor 资源池
```

它并不管理 `avail` 或 `used` ring 的槽位。

---

# 4. 一次 block 请求为什么需要 3 个 descriptor

xv6 一次 VirtIO block 请求由三个部分组成：

```text
① request header
② data buffer
③ completion status
```

所以通常需要三个 descriptor：

```text
desc A
   │
   ▼
desc B
   │
   ▼
desc C
```

分别指向：

```text
request header
      ↓
b->data
      ↓
status
```

假设：

```c
alloc3_desc(idx);
```

得到：

```text
idx[0] = 1
idx[1] = 2
idx[2] = 5
```

那么本次请求使用：

```text
desc[1] → desc[2] → desc[5]
```

注意：

> 三个 descriptor 不要求在数组中连续。

---

# 5. `ops[]` 保存磁盘命令头

```c
struct virtio_blk_req ops[NUM];
```

请求头大致包含：

```c
struct virtio_blk_req {
  uint32 type;
  uint32 reserved;
  uint64 sector;
};
```

例如读取 xv6 block 100。

如果：

```text
BSIZE = 1024 bytes
VirtIO sector = 512 bytes
```

则：

```text
xv6 block 100
    ↓
VirtIO sector 200
```

请求头可能为：

```text
ops[1]

type   = VIRTIO_BLK_T_IN
sector = 200
```

第一个 descriptor：

```c
desc[1].addr = (uint64)&disk.ops[1];
desc[1].len = sizeof(struct virtio_blk_req);
desc[1].flags = VRING_DESC_F_NEXT;
desc[1].next = 2;
```

因此：

```text
desc[1]
   │
   └────► ops[1]
             │
             ├── READ
             └── sector = 200
```

---

# 6. 为什么 `ops[]` 有 NUM 个元素

一次请求只需要一个 request header。

`ops[NUM]` 并不是因为每个 descriptor 都需要一个 header。

xv6 只是采用一个方便的固定映射：

```text
descriptor chain head = i
          ↓
       ops[i]
```

因此如果 chain head 是：

```text
1
```

则请求头就存放于：

```text
ops[1]
```

这样避免动态分配请求头内存。

---

# 7. 第二个 descriptor 指向 `b->data`

假设：

```text
idx[1] = 2
```

那么：

```c
desc[2].addr = (uint64)b->data;
desc[2].len = BSIZE;
desc[2].next = 5;
```

关系：

```text
desc[2]
   │
   └────► b->data[BSIZE]
```

---

# 8. `VRING_DESC_F_WRITE` 的方向

这个 flag 很容易误解。

```c
VRING_DESC_F_WRITE
```

不是：

```text
“磁盘 WRITE”
```

它表示：

> 设备可以向 descriptor 指向的内存写入数据。

因此：

## 磁盘 READ

```text
disk → memory
```

设备要写：

```text
b->data
```

所以设置：

```c
VRING_DESC_F_WRITE
```

---

## 磁盘 WRITE

```text
memory → disk
```

设备只是读取：

```text
b->data
```

因此不设置 `VRING_DESC_F_WRITE`。

总结：

| 磁盘操作 | `b->data` 数据方向 | `VRING_DESC_F_WRITE` |
|---|---|---|
| READ | device → memory | 设置 |
| WRITE | memory → device | 不设置 |

这个 flag 永远站在：

```text
device 对 memory 的访问方向
```

来理解。

---

# 9. 第三个 descriptor 指向 completion status

假设：

```text
idx[2] = 5
```

第三个 descriptor 指向的不是：

```text
info[5].status
```

而是：

```text
info[idx[0]].status
```

也就是：

```text
info[1].status
```

因为整个请求都以：

```text
descriptor chain head
```

作为 request ID。

所以：

```c
disk.info[1].status = 0xff;

desc[5].addr = (uint64)&disk.info[1].status;
desc[5].len = 1;
desc[5].flags = VRING_DESC_F_WRITE;
```

设备完成后会向这里写状态，例如：

```text
0 = success
```

于是：

```text
desc[5]
   │
   └────► info[1].status
```

---

# 10. `info[]` 是从 VirtIO request ID 找回 xv6 请求的映射

```c
struct {
  struct buf *b;
  char status;
} info[NUM];
```

假设：

```text
chain head = 1
```

驱动会记录：

```c
disk.info[1].b = b;
```

于是：

```text
request ID = 1
     │
     ▼
 info[1]
 ┌──────────────┐
 │ b      ──────────► struct buf
 │ status       │
 └──────────────┘
```

以后设备完成请求时，只需要返回：

```text
head descriptor index = 1
```

驱动就可以：

```c
struct buf *b = disk.info[1].b;
```

找回原来的 buffer。

因此：

> `info[]` 是 VirtIO descriptor head ID 到 xv6 `struct buf *` 的映射表。

---

# 11. descriptor chain 的完整形态

假设：

```text
idx[0] = 1
idx[1] = 2
idx[2] = 5
```

并且执行磁盘 READ。

最终：

```text
desc[1]
┌──────────────────────────┐
│ addr = &ops[1]           │
│ len = sizeof(req)        │
│ flags = NEXT             │
│ next = 2                 │
└────────────┬─────────────┘
             │
             ▼
desc[2]
┌──────────────────────────┐
│ addr = b->data           │
│ len = BSIZE              │
│ flags = WRITE | NEXT     │
│ next = 5                 │
└────────────┬─────────────┘
             │
             ▼
desc[5]
┌──────────────────────────┐
│ addr = &info[1].status   │
│ len = 1                  │
│ flags = WRITE            │
└──────────────────────────┘
```

其中：

```text
desc[1] → request header
desc[2] → actual block data
desc[5] → completion status
```

---

# 12. `avail` 是 driver → device 的提交队列

仅仅填好 `desc[]` 并不会让设备主动发现请求。

设备不会遍历：

```text
desc[0..NUM-1]
```

寻找新工作。

驱动必须把：

```text
descriptor chain head
```

放到 `avail` ring。

可以把它理解为：

```text
desc[]   = 工作具体怎么做
avail[]  = 哪些工作现在可以开始做
```

---

# 13. `avail->ring[]` 只保存 chain head

如果 descriptor chain 是：

```text
desc[1] → desc[2] → desc[5]
```

那么 driver 只提交：

```text
1
```

例如：

```c
disk.avail->ring[disk.avail->idx % NUM] = 1;
```

不会写：

```text
1, 2, 5
```

因为设备拿到：

```text
1
```

以后可以通过 descriptor 的：

```text
NEXT + next
```

自己遍历：

```text
1 → 2 → 5
```

---

# 14. `avail->idx` 是逻辑生产序号

必须区分：

```text
descriptor index
```

和：

```text
ring logical index
```

例如：

```text
NUM = 8
avail->idx = 13
```

这并不意味着：

```text
desc[13]
```

而是表示：

> driver 已经发布到第 13 个逻辑位置。

实际访问 ring：

```c
disk.avail->ring[
    disk.avail->idx % NUM
] = idx[0];
```

即：

```text
13 % 8 = 5
```

所以：

```text
avail->ring[5] = 1
```

然后：

```c
disk.avail->idx++;
```

变成：

```text
14
```

---

# 15. 为什么 ring 使用 `% NUM`

ring 会循环复用槽位：

```text
logical position:

0 1 2 3 4 5 6 7 8 9 10 ...
                │ │
physical slot:
0 1 2 3 4 5 6 7 0 1  2 ...
```

因此：

```text
logical index % NUM
```

得到实际数组槽位。

这与 descriptor 编号是完全不同的 index 空间。

---

# 16. `desc[]` 与 `avail[]` 的区别

这是理解 VirtIO queue 最重要的区别之一。

## `desc[]`

描述：

```text
“请求要访问哪块内存、多大、下一个 descriptor 是谁”
```

属于：

```text
工作内容
```

---

## `avail->ring[]`

描述：

```text
“哪个 descriptor chain 已准备好，可以执行”
```

属于：

```text
待办队列
```

例如：

```text
avail->ring[x] = 1
                   │
                   ▼
                desc[1]
                   │
                   ▼
                desc[2]
                   │
                   ▼
                desc[5]
```

---

# 17. `used` 是 device → driver 的完成队列

`avail` 与 `used` 正好相反：

```text
avail:
driver → device

used:
device → driver
```

`used` 的含义：

> 哪些 descriptor chain 已经处理完成。

设备完成：

```text
desc[1] → desc[2] → desc[5]
```

以后，会向 `used` ring 写入类似：

```text
id = 1
```

仍然只需要返回：

```text
chain head
```

因为：

```text
1
↓
info[1]
↓
buf
```

已经足够让 xv6 找回请求。

---

# 18. `used_idx` 是 driver 自己的消费位置

设备有：

```c
disk.used->idx
```

表示：

> device 已经产生了多少个完成记录。

驱动有：

```c
disk.used_idx
```

表示：

> xv6 driver 自己已经消费到哪里。

例如：

```text
used->idx = 10
used_idx  = 8
```

表示还有两个 completion 没处理：

```text
logical entry 8
logical entry 9
```

driver 会访问：

```c
disk.used->ring[
    disk.used_idx % NUM
]
```

---

# 19. `used_idx` 不是 descriptor 编号

例如：

```text
disk.used_idx = 37
NUM = 8
```

访问：

```text
37 % 8 = 5
```

得到：

```text
used->ring[5]
```

然后：

```text
used->ring[5].id
```

才是真正的：

```text
descriptor chain head
```

例如：

```text
37
 │
 │ % 8
 ▼
used->ring[5]
 │
 │ .id
 ▼
1
 │
 ▼
info[1]
```

所以这里存在三种不同概念：

```text
logical ring index
physical ring slot
descriptor index
```

不要混淆。

---

# 20. 一次请求如何分布在全部数据结构中

假设：

```text
NUM = 8

idx[0] = 1
idx[1] = 2
idx[2] = 5

blockno = 100
```

请求头：

```text
ops[1]

type   = READ
sector = 200
```

descriptor：

```text
desc[1]
  addr  = &ops[1]
  len   = sizeof(req)
  flags = NEXT
  next  = 2

desc[2]
  addr  = b->data
  len   = BSIZE
  flags = WRITE | NEXT
  next  = 5

desc[5]
  addr  = &info[1].status
  len   = 1
  flags = WRITE
```

bookkeeping：

```text
info[1]

b      = 当前 struct buf *
status = 0xff
```

资源状态：

```text
free[1] = 0
free[2] = 0
free[5] = 0
```

提交：

```text
avail->ring[avail->idx % NUM] = 1
avail->idx++
```

于是设备：

```text
avail
  │
  ▼
  1
  │
  ▼
desc[1]
  │
  ▼
desc[2]
  │
  ▼
desc[5]
```

完成后设备：

```text
used ring
    │
    ▼
  id = 1
```

driver：

```text
1
↓
info[1]
↓
b
```

---

# 21. chain head 本质上就是 request ID

xv6 的设计中：

```text
idx[0]
```

不仅是第一个 descriptor 的编号。

它实际上同时扮演：

```text
request identity
```

的作用。

如果：

```text
head = i
```

那么常见关系为：

```text
desc[i]
ops[i]
info[i]
```

并且：

```text
avail ring 提交 i
used ring 返回 i
```

所以可以画成：

```text
                      request ID = i
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
           desc[i]       ops[i]       info[i]
              │                          │
              ▼                          └──► buf
      descriptor chain
              │
              ▼
     avail->ring[...] = i
              │
              ▼
           DEVICE
              │
              ▼
     used->ring[...].id = i
```

---

# 22. `virtio_disk_rw()` 的核心工作

忽略 sleep/wakeup 后，可以把函数理解成：

```c
virtio_disk_rw(b, write)
{
    int idx[3];

    // 1. 分配三个 descriptor
    alloc3_desc(idx);

    // 假设：
    // idx[0] = 1
    // idx[1] = 2
    // idx[2] = 5

    // 2. 准备 request header
    disk.ops[1].type =
        write ? WRITE : READ;

    disk.ops[1].sector =
        b->blockno * sectors_per_block;

    // 3. descriptor 1 -> request header
    desc[1].addr = &ops[1];
    desc[1].len = sizeof(req);
    desc[1].flags = NEXT;
    desc[1].next = 2;

    // 4. descriptor 2 -> data buffer
    desc[2].addr = b->data;
    desc[2].len = BSIZE;
    desc[2].flags = NEXT | ...;
    desc[2].next = 5;

    // 5. descriptor 5 -> completion status
    info[1].status = 0xff;

    desc[5].addr = &info[1].status;
    desc[5].len = 1;
    desc[5].flags = WRITE;

    // 6. 保存 request ID -> buf 映射
    info[1].b = b;

    // 7. 向 avail ring 发布 request ID
    avail->ring[avail->idx % NUM] = 1;

    io_fence();

    avail->idx += 1;

    io_fence();

    // 8. MMIO notify device
    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
}
```

核心转换过程：

```text
struct buf *
    ↓
构造 request header
    ↓
构造 descriptor chain
    ↓
保存 head → buf 映射
    ↓
将 head 发布到 avail ring
    ↓
notify device
```

---

# 23. `io_fence()` 是什么

`io_fence()` 是：

```text
memory ordering / memory visibility barrier
```

可以理解为：

> 确保 fence 前面的相关内存访问按照协议要求先完成并对设备可见，再允许后面的发布动作继续。

它解决的是：

```text
CPU / 编译器可能重排内存访问
```

以及：

```text
CPU 与 DMA 设备之间需要明确的内存可见顺序
```

的问题。

---

# 24. 为什么 VirtIO 提交请求需要 `io_fence()`

提交请求的逻辑顺序必须是：

```text
① 写 ops[]
② 写 desc[]
③ 写 info[]
④ 写 avail->ring[]
⑤ 更新 avail->idx
⑥ MMIO notify
```

设备把：

```text
avail->idx
```

看作“有多少请求已经正式发布”的依据。

因此：

> 在让设备看到新的 `avail->idx` 之前，必须确保对应 descriptor chain 和 avail ring entry 已经全部准备好。

---

# 25. 如果没有 fence 会发生什么

C 代码可能写成：

```c
disk.avail->ring[...] = head;
disk.avail->idx++;
```

从程序员视角：

```text
先写 ring
再增加 idx
```

但如果缺乏正确的 memory ordering，设备可能先观察到：

```text
avail->idx 已变化
```

于是认为：

```text
有新请求
```

接着读取：

```text
avail->ring
desc[]
ops[]
```

而其中某些写入可能还没有按照协议要求对设备可见。

结果就是设备可能看到：

```text
“请求已经发布”
```

但请求内容还不是完整的新值。

---

# 26. `avail->idx` 可以理解为发布点

可以把请求构建过程理解成：

```text
写 ops
   ↓
写 desc
   ↓
写 info/status
   ↓
写 avail->ring
   ↓
 io_fence()
   ↓
更新 avail->idx
```

关键顺序：

```text
request contents
      happens-before
avail->idx publication
```

也就是说：

```text
avail->idx 增加
```

相当于告诉设备：

> 前面的请求已经全部准备完成，可以消费。

---

# 27. notify 前为什么还可能需要 fence

更新：

```text
avail->idx
```

之后，driver 会通过 MMIO：

```c
*R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
```

通知设备。

逻辑要求：

```text
avail->idx 已经对设备可见
        ↓
然后才能通知设备
```

因此可使用另一个 fence：

```text
avail->idx++
    ↓
io_fence()
    ↓
MMIO notify
```

避免出现：

```text
设备先收到 notify
        ↓
读取 avail->idx
        ↓
却仍看到旧值
```

这样的顺序问题。

---

# 28. `io_fence()` 不是什么

`io_fence()` 不是：

```text
等待磁盘操作完成
```

不是：

```text
sleep
```

不是：

```text
lock
```

不是：

```text
disable interrupt
```

也不应简单理解成：

```text
清空 CPU cache
```

它解决的是：

> 内存访问顺序与可见性。

---

# 29. `io_fence()` 的心智模型

可以把：

```c
io_fence();
```

脑补成：

> “在继续执行协议的下一阶段之前，确保前面那些必须先发生的内存访问，已经按照要求完成并对设备可见。”

在请求提交阶段：

```text
写请求内容
     ↓
io_fence
     ↓
发布请求
     ↓
io_fence
     ↓
notify
```

---

# 30. 整个 `disk` 数据结构的关系图

```text
                         struct disk
                              │
          ┌───────────────────┼────────────────────┐
          │                   │                    │
          ▼                   ▼                    ▼

       desc[]              avail ring           used ring
    VirtIO共享             driver→device         device→driver

     desc[1]                    │                    ▲
       │                        │                    │
       │                        │ publish 1          │ completed 1
       │                        ▼                    │
       └──────────────────── Request ID = 1 ────────┘
                │
                │
         ┌──────┴────────┐
         ▼               ▼
      ops[1]           info[1]
    ┌────────┐       ┌────────────┐
    │ READ   │       │ b ─────────────► struct buf
    │ sector │       │ status     │       │
    └────────┘       └────────────┘       │
                                          ▼
                                      b->data
```

descriptor chain：

```text
desc[1] ─────► ops[1]
    │
    ▼
desc[2] ─────► b->data
    │
    ▼
desc[5] ─────► info[1].status
```

---

# 31. 最终需要记住的关系

## `free[i] ↔ desc[i]`

```text
free[i]
```

决定：

```text
desc[i]
```

是否可以分配。

---

## `ops[i]` 通常绑定 descriptor chain head `i`

```text
head = i
   ↓
ops[i]
```

保存 block request header。

---

## `info[i]` 同样绑定 chain head `i`

```text
head = i
   ↓
info[i]
```

保存：

```text
buf 指针
completion status
```

---

## `avail->ring[x]` 保存 descriptor chain head

```text
avail->ring[x] = i
```

含义：

> device，请处理从 `desc[i]` 开始的 chain。

---

## `used->ring[x].id` 返回 descriptor chain head

```text
used->ring[x].id = i
```

含义：

> 从 `desc[i]` 开始的 request 已完成。

---

## `used_idx` 是 used ring 的逻辑消费位置

它不是 descriptor 编号。

访问时：

```text
used_idx % NUM
```

得到 used ring 的物理槽位。

---

## `io_fence()` 保证请求发布顺序

核心顺序：

```text
准备 request 数据
      ↓
准备 descriptor
      ↓
写 avail ring
      ↓
io_fence
      ↓
增加 avail->idx
      ↓
io_fence
      ↓
MMIO notify
```

---

# 32. 一句话总结 `virtio_disk_rw()`

`virtio_disk_rw()` 的本质是：

> 从 `desc[]` 中临时分配三个 descriptor，把“磁盘命令、数据 buffer、完成状态”串成一条 descriptor chain；以第一个 descriptor 的编号作为整个请求的 ID，通过 `ops[]` 和 `info[]` 保存请求相关信息，再把这个 head ID 放进 `avail` ring，并借助 `io_fence()` 保证 descriptor 内容、ring 发布和 MMIO notify 之间的内存可见顺序。

最终的数据流可以压缩成：

```text
struct buf
    │
    ▼
ops[head]
    │
    ▼
descriptor chain
    │
    ▼
avail ring
    │
    ▼
VirtIO device
    │
    ▼
used ring
    │
    ▼
head ID
    │
    ▼
info[head]
    │
    ▼
struct buf
```
