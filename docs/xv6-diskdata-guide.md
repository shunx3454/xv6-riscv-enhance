# xv6 Disk Data Guide

> 面向 MIT xv6-riscv 的文件系统底层数据布局与核心概念速查。
>
> 本文重点解释：磁盘 block、block 编号、inode/dinode、inode 编号、inode block、bitmap、目录数据、普通文件数据、直接索引、一级间接索引、设备编号，以及这些结构之间如何连接起来。

---

## 1. 先看 xv6 文件系统的整体层次

从用户程序执行：

```c
open()
read()
write()
close()
```

到底层磁盘，大致经过：

```text
User Program
    │
    │ syscall
    ▼
sys_open / sys_read / sys_write
    │
    ▼
struct file / fd
    │
    ▼
struct inode
    │
    ├── pathname / directory
    │
    ├── bmap()
    │
    ▼
filesystem block
    │
    ▼
buffer cache
    │
    ▼
logging / disk driver
    │
    ▼
Disk
```

最值得记住的一条数据链：

```text
fd
 ↓
struct file
 ↓
struct inode
 ↓
文件逻辑 block
 ↓
bmap()
 ↓
磁盘 block number
 ↓
buffer cache
 ↓
disk
```

路径解析则是另一条链：

```text
pathname
 ↓
directory inode
 ↓
dirent(name → inum)
 ↓
inode number
 ↓
target inode
```

---

# 2. xv6 的磁盘是按 block 管理的

标准 xv6-riscv：

```c
#define BSIZE 1024
```

所以：

```text
1 filesystem block = 1024 bytes = 1 KiB
```

整个文件系统可以看成一个 block 数组：

```text
block 0
block 1
block 2
block 3
...
block N-1
```

因此 **block number（block 编号）** 就是某个 1 KiB block 在文件系统中的编号。

例如：

```text
blockno = 100
```

对应的磁盘字节偏移大致是：

```text
100 × 1024 = 102400 bytes
```

也就是说：

```text
block 100
=
byte 102400 ~ 103423
```

---

# 3. xv6 磁盘布局

典型 xv6 文件系统布局：

```text
block 0
┌─────────────────────────┐
│ Boot Block              │
├─────────────────────────┤ block 1
│ Super Block             │
├─────────────────────────┤
│ Log Blocks              │
│ ...                     │
├─────────────────────────┤
│ Inode Blocks            │
│ ...                     │
├─────────────────────────┤
│ Bitmap Blocks           │
│ ...                     │
├─────────────────────────┤
│ Data Blocks             │
│ ...                     │
└─────────────────────────┘
```

可以简单记成：

```text
boot
 ↓
superblock
 ↓
log
 ↓
inode blocks
 ↓
bitmap blocks
 ↓
data blocks
```

---

# 4. Superblock：文件系统的“地图”

xv6 中：

```c
struct superblock {
  uint magic;
  uint size;       // 总 block 数
  uint nblocks;    // data block 数
  uint ninodes;    // inode 数
  uint nlog;       // log block 数

  uint logstart;   // log 起始 block
  uint inodestart; // inode 区起始 block
  uint bmapstart;  // bitmap 区起始 block
};
```

它告诉内核：

```text
整个文件系统多大？
log 在哪里？
inode 表在哪里？
bitmap 在哪里？
data 区从哪里开始？
```

所以可以把 superblock 看成：

```text
filesystem metadata map
```

---

# 5. inode 是“文件本体”的核心元数据

Unix 文件系统里，真正代表一个文件的不是文件名，而是：

```text
inode
```

文件名只是目录中的一个映射：

```text
"name" → inode number
```

例如：

```text
"a.txt" → inode 35
```

真正记录文件大小、文件类型、数据块位置的是 inode 35。

---

# 6. 磁盘 inode：struct dinode

磁盘上真正持久化的是：

```c
struct dinode {
  short type;              // File type
  short major;             // Major device number (T_DEVICE only)
  short minor;             // Minor device number (T_DEVICE only)
  short nlink;             // Number of links
  uint size;               // File size in bytes
  uint addrs[NDIRECT + 1]; // Data block addresses
};
```

标准 xv6-riscv：

```c
#define NDIRECT 12
```

因此：

```text
addrs[0] ~ addrs[11]  = 12 个直接地址
addrs[12]             = 1 个一级间接索引块地址
```

---

# 7. 一个 dinode 多大？

字段大小：

```text
type      2 bytes
major     2
minor     2
nlink     2
          ----
           8

size      4
          ----
          12

addrs[13]
13 × 4 = 52

总计：
12 + 52 = 64 bytes
```

因此：

```text
sizeof(struct dinode) = 64 bytes
```

---

# 8. 一个 inode block 能放几个 inode？

一个 block：

```text
1024 bytes
```

一个 dinode：

```text
64 bytes
```

所以：

```text
1024 / 64 = 16
```

xv6：

```c
#define IPB (BSIZE / sizeof(struct dinode))
```

即：

```text
IPB = Inodes Per Block = 16
```

一个 inode block 的布局：

```text
1024-byte inode block

┌────────────────────┐
│ dinode slot 0  64B │
├────────────────────┤
│ dinode slot 1  64B │
├────────────────────┤
│ dinode slot 2  64B │
├────────────────────┤
│ ...                │
├────────────────────┤
│ dinode slot 15 64B │
└────────────────────┘
```

---

# 9. inode number 是什么？

inode number，简称：

```text
inum
```

表示：

```text
inode table 中某个 inode 的编号
```

例如：

```text
inode 1
inode 2
inode 3
...
inode 35
```

注意：

```text
inode number ≠ disk block number
```

它们是两套不同的编号体系。

标准 xv6：

```c
#define ROOTINO 1
```

所以：

```text
inode 1 = 根目录 "/"
```

inode 0 通常不作为普通有效 inode 使用。

---

# 10. inode number 如何定位到磁盘上的 inode block？

xv6：

```c
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)
```

因为：

```text
IPB = 16
```

所以：

```text
inode 所在磁盘 block
=
sb.inodestart + inum / 16
```

而 inode 在该 block 内的槽位：

```text
slot = inum % 16
```

---

## 示例：inode 35

假设：

```text
sb.inodestart = 32
inum = 35
```

计算：

```text
35 / 16 = 2
```

所以：

```text
inode block = 32 + 2 = block 34
```

再计算：

```text
35 % 16 = 3
```

所以 inode 35 位于：

```text
block 34

┌───────────────────┐
│ slot 0: inode 32  │
├───────────────────┤
│ slot 1: inode 33  │
├───────────────────┤
│ slot 2: inode 34  │
├───────────────────┤
│ slot 3: inode 35  │ ←
├───────────────────┤
│ ...               │
└───────────────────┘
```

因此可以记：

```text
inum / IPB
    ↓
找 inode block

inum % IPB
    ↓
找 block 内 slot
```

---

# 11. 内存 inode 和磁盘 dinode 不是同一个结构

磁盘：

```c
struct dinode
```

内存：

```c
struct inode
```

内存 inode 通常还包含：

```text
dev
inum
ref
lock
valid
...
```

关系：

```text
Disk
  │
  │ struct dinode
  ▼
inode block
  │
  │ ilock / read
  ▼
RAM
  │
  └── struct inode
```

磁盘 dinode 是持久化格式。

内存 inode 是运行时对象。

---

# 12. inode 的身份实际上是 (dev, inum)

仅有 inode number 只在某个文件系统设备内部唯一。

因此更准确地说：

```text
(dev, inum)
```

共同标识一个 inode。

例如：

```text
(dev=1, inum=35)
```

和：

```text
(dev=2, inum=35)
```

可以代表两个完全不同的文件。

---

# 13. 什么是 inode->dev？

`ip->dev` 表示：

```text
这个 inode 属于哪个文件系统设备
```

可以简单理解为：

```text
这个 inode 在哪个磁盘/块设备上的文件系统中
```

它和 dinode 里的：

```text
major
minor
```

不是一回事。

---

# 14. major / minor 是什么？

只有：

```text
type == T_DEVICE
```

时，major/minor 才主要有意义。

概念上：

```text
major
 ↓
选择设备驱动

minor
 ↓
该驱动下的具体设备实例
```

例如设备文件：

```text
/dev/console
```

读取时不是正常走：

```text
inode addrs[]
 ↓
data block
```

而更接近：

```text
device inode
 ↓
major
 ↓
devsw[major]
 ↓
driver read/write
```

因此：

```text
ip->dev
    = inode 所属文件系统设备

major/minor
    = T_DEVICE inode 所代表的设备
```

---

# 15. inode 的 addrs[] 不存文件内容

最关键的一点：

```c
uint addrs[NDIRECT + 1];
```

里面保存的是：

```text
磁盘 block number
```

而不是文件内容。

例如：

```text
inode 35

size = 2500
addrs[0] = 100
addrs[1] = 205
addrs[2] = 306
```

表示：

```text
文件 offset 0~1023
        ↓
disk block 100

文件 offset 1024~2047
        ↓
disk block 205

文件 offset 2048~2499
        ↓
disk block 306
```

因此：

```text
inode = 文件元数据 + 文件数据所在位置
```

而不是：

```text
inode = 文件内容本身
```

---

# 16. 什么叫“直接索引”？

标准 xv6：

```text
addrs[0] ~ addrs[11]
```

是 12 个直接 block 地址。

例如：

```text
inode
┌─────────────────────┐
│ addrs[0]  = 100     │────────→ data block 100
│ addrs[1]  = 205     │────────→ data block 205
│ addrs[2]  = 300     │────────→ data block 300
│ ...                 │
│ addrs[11] = 900     │────────→ data block 900
└─────────────────────┘
```

叫“直接”的原因：

```text
inode.addrs[n]
      ↓
直接得到
      ↓
data block number
```

只有一层映射。

---

# 17. 直接索引能覆盖多大的文件？

12 个直接 block：

```text
12 × 1024
= 12288 bytes
= 12 KiB
```

所以文件前 12 KiB：

```text
逻辑 block 0 ~ 11
```

可以全部直接通过：

```text
addrs[0] ~ addrs[11]
```

找到。

---

# 18. 为什么需要一级间接索引？

如果只存 12 个 block number，文件最多只有 12 KiB。

如果直接扩大 inode：

```c
uint addrs[1000];
```

又会导致每个 inode 非常大，大量小文件浪费空间。

所以 xv6 使用：

```text
一级间接索引
```

核心思想：

```text
inode 不直接存所有 data block 地址，
而让其中一个 addrs[] 指向“地址表 block”。
```

---

# 19. addrs[12] 是一级间接索引地址

标准 xv6：

```text
addrs[0] ~ addrs[11]
    ↓
data blocks

addrs[12]
    ↓
indirect block
```

图：

```text
inode
│
├── addrs[0]  ─────────────→ data block
├── addrs[1]  ─────────────→ data block
├── ...
├── addrs[11] ─────────────→ data block
│
└── addrs[12]
        │
        ▼
   indirect block
   ┌───────────────────┐
   │ uint[0] = 500     │────→ data block 500
   │ uint[1] = 620     │────→ data block 620
   │ uint[2] = 750     │────→ data block 750
   │ ...               │
   └───────────────────┘
```

因此：

```text
直接索引：
inode → data block

一级间接：
inode → block-number table → data block
```

之所以叫“间接”，就是因为多查了一层。

---

# 20. 一个 indirect block 能存多少个 block number？

一个 block：

```text
1024 bytes
```

一个 block number：

```c
uint
```

占：

```text
4 bytes
```

所以：

```text
1024 / 4 = 256
```

xv6：

```c
#define NINDIRECT (BSIZE / sizeof(uint))
```

因此：

```text
NINDIRECT = 256
```

一个 indirect block 可以保存：

```text
256 个 data block number
```

---

# 21. xv6 单文件最大大小

数据块数量：

```text
12 个直接块
+
256 个一级间接数据块
=
268 个数据块
```

所以：

```text
MAXFILE = 12 + 256 = 268 blocks
```

最大数据量：

```text
268 × 1024
= 274432 bytes
≈ 268 KiB
```

---

# 22. 一级间接索引完整例子

假设文件需要 15 个 block。

前 12 个：

```text
逻辑 block 0  → addrs[0]
逻辑 block 1  → addrs[1]
...
逻辑 block 11 → addrs[11]
```

剩余：

```text
逻辑 block 12
逻辑 block 13
逻辑 block 14
```

假设：

```text
addrs[12] = 400
```

说明：

```text
block 400 = indirect block
```

里面：

```text
block 400

┌──────────────────────┐
│ uint[0] = 700        │
├──────────────────────┤
│ uint[1] = 800        │
├──────────────────────┤
│ uint[2] = 950        │
├──────────────────────┤
│ uint[3] = 0          │
│ ...                  │
└──────────────────────┘
```

因此：

```text
文件逻辑 block 12
    ↓
addrs[12]
    ↓
block 400
    ↓
uint[0] = 700
    ↓
data block 700
```

文件逻辑 block 13：

```text
addrs[12]
    ↓
block 400
    ↓
uint[1] = 800
    ↓
data block 800
```

文件逻辑 block 14：

```text
addrs[12]
    ↓
block 400
    ↓
uint[2] = 950
    ↓
data block 950
```

---

# 23. bmap() 做什么？

核心接口：

```c
bmap(ip, bn)
```

其中：

```text
bn = 文件内部的逻辑 block 编号
```

它的职责：

```text
文件逻辑 block number
        ↓
       bmap()
        ↓
磁盘物理 block number
```

逻辑：

```text
                 bn
                  │
             bn < 12 ?
             /       \
           yes       no
           │          │
           ▼          ▼
    ip->addrs[bn]   bn -= 12
                      │
                      ▼
                ip->addrs[12]
                      │
                      ▼
                indirect block
                      │
                      ▼
                    a[bn]
```

---

# 24. 普通文件 inode 的 data block 里是什么？

对于：

```text
type == T_FILE
```

data block 存放的是：

```text
普通文件的原始 bytes
```

例如 `hello.txt`：

```text
inode 35
│
├── addrs[0] = 700
│       │
│       ▼
│   block 700
│   ┌────────────────────┐
│   │ "Hello xv6!\n..."  │
│   │ 普通文件字节       │
│   └────────────────────┘
│
└── addrs[1] = 850
        │
        ▼
    后续文件数据
```

文件系统不会因为：

```text
.txt
.c
.jpg
ELF
```

而改变 inode 的基本存储方式。

对于普通文件，文件系统主要只认为它是：

```text
一串 bytes
```

---

# 25. 目录 inode 的 data block 里是什么？

目录也是 inode：

```text
type == T_DIR
```

而且它同样有：

```text
size
addrs[]
```

区别在于：

```text
目录 data block 存的不是用户文件 bytes，
而是一组 struct dirent。
```

xv6：

```c
struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
```

标准 xv6：

```c
#define DIRSIZ 14
```

因此：

```text
sizeof(struct dirent)
=
2 + 14
=
16 bytes
```

---

# 26. 一个目录的数据长什么样？

例如目录：

```text
/home
```

里面有：

```text
.
..
a.txt
test
hello.c
```

假设 `/home` 是 inode 20：

```text
inode 20

type = T_DIR
addrs[0] = 500
```

那么 block 500 可能逻辑上是：

```text
block 500

┌───────────────────────────┐
│ inum=20  name="."         │
├───────────────────────────┤
│ inum=1   name=".."        │
├───────────────────────────┤
│ inum=35  name="a.txt"     │
├───────────────────────────┤
│ inum=42  name="test"      │
├───────────────────────────┤
│ inum=51  name="hello.c"   │
└───────────────────────────┘
```

所以：

```text
directory data
=
name → inode number
```

---

# 27. 文件名不存储在 inode 中

例如目录项：

```text
"a.txt" → inode 35
```

inode 35 本身只知道：

```text
type
size
nlink
addrs[]
...
```

它不知道自己叫：

```text
"a.txt"
```

这也是 hard link 能成立的原因：

```text
"a.txt" ──┐
          │
          ▼
       inode 35
          ▲
          │
"b.txt" ──┘
```

两个不同名字可以指向同一个 inode。

---

# 28. 普通文件 inode 和目录 inode 的结构完全相同

两者都使用：

```c
struct dinode
```

也都使用：

```text
12 个直接块
+
1 个一级间接块
```

区别只有 data block 内容的语义：

| inode type | data block 内容 |
|---|---|
| `T_FILE` | 文件原始字节 |
| `T_DIR` | `struct dirent[]` |
| `T_DEVICE` | 主要通过 major/minor 调设备驱动 |

因此：

```text
                 dinode
                   │
                 addrs[]
                   │
          ┌────────┴────────┐
          │                 │
       T_FILE             T_DIR
          │                 │
          ▼                 ▼
     data blocks        data blocks
          │                 │
      raw bytes          dirent[]
```

---

# 29. 目录 inode 的 size 是什么？

普通文件：

```text
size = 文件字节数
```

目录：

```text
size = 目录文件数据的字节数
```

因为目录内容本质是：

```text
struct dirent[]
```

如果有 10 个目录项槽位：

```text
10 × 16 bytes
= 160 bytes
```

则目录 inode 的 size 可以反映这段目录数据的长度。

---

# 30. dirlookup() 本质上是在“读目录文件”

目录查找：

```c
dirlookup(dp, name, ...)
```

概念上就是：

```text
for 每个目录项：
    从目录 inode 中 readi()
    读出一个 struct dirent
    检查 de.name
    如果匹配：
        获取 de.inum
```

也就是说：

```text
目录查找
=
读取一个格式特殊的文件
```

---

# 31. 路径解析示例：/home/a.txt

解析：

```text
/home/a.txt
```

大致过程：

```text
ROOTINO = 1
    │
    ▼
root inode
    │
    │ root data block: dirent[]
    ▼
查 "home"
    │
    ▼
inum = 20
    │
    ▼
inode 20 (/home)
    │
    │ /home data block: dirent[]
    ▼
查 "a.txt"
    │
    ▼
inum = 35
    │
    ▼
inode 35 (普通文件)
```

然后读取文件内容：

```text
inode 35
   │
   │ addrs[] / bmap()
   ▼
data blocks
   │
   ▼
"a.txt" 真正内容
```

---

# 32. Bitmap：记录 block 是否已占用

xv6 使用 block bitmap 记录：

```text
某个 filesystem block 是否已经被使用
```

规则：

```text
1 bit ↔ 1 filesystem block
```

例如：

```text
blockno:  0 1 2 3 4 5 6 7
bitmap:   1 1 1 1 0 1 0 0
```

可理解为：

```text
block 0 used
block 1 used
block 2 used
block 3 used
block 4 free
block 5 used
block 6 free
block 7 free
```

---

# 33. 一个 bitmap block 能记录多少 filesystem blocks？

一个 bitmap block：

```text
1024 bytes
```

每 byte：

```text
8 bits
```

所以：

```text
1024 × 8
= 8192 bits
```

因为：

```text
1 bit ↔ 1 filesystem block
```

所以：

```text
1 bitmap block
↔
8192 filesystem blocks
```

xv6：

```c
#define BPB (BSIZE * 8)
```

即：

```text
BPB = 8192
```

---

# 34. 如何找到某个 block 对应的 bitmap block？

xv6：

```c
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)
```

也就是说：

```text
blockno / 8192
    ↓
第几个 bitmap block
```

加上：

```text
sb.bmapstart
```

得到真正的磁盘 block number。

---

# 35. 如何找到 bitmap block 里的具体 bit？

假设：

```text
blockno = 10000
```

先：

```text
10000 / 8192 = 1
```

说明在：

```text
第 1 个 bitmap block
```

再：

```text
10000 % 8192 = 1808
```

说明是该 bitmap block 中：

```text
bit 1808
```

进一步：

```text
byte index = 1808 / 8 = 226
bit index  = 1808 % 8 = 0
```

所以：

```text
bitmap block
    │
    ▼
data[226]
    │
    ▼
bit 0
```

就是 filesystem block 10000 的分配状态。

---

# 36. Bitmap 是否只表示 data block？

最准确的说法：

```text
一个 bit 对应一个 filesystem block number
```

不仅仅在编号意义上对应 data 区。

`mkfs` 创建文件系统时会把：

```text
boot block
superblock
log blocks
inode blocks
bitmap blocks
```

这些已经被 metadata 占用的 block 对应的 bit 标成：

```text
used
```

因此之后：

```c
balloc()
```

扫描 bitmap 时，只会找到尚未被占用的 block。

这些最终通常用于：

```text
文件/目录 data block
indirect block
```

---

# 37. xv6 没有 inode bitmap

这是 xv6 和很多成熟 Unix 文件系统不同的地方。

xv6 有：

```text
block bitmap
```

但没有单独：

```text
inode bitmap
```

空闲 inode 的判断方式：

```text
dinode.type == 0
```

所以：

```text
block 是否空闲
    ↓
bitmap bit

inode 是否空闲
    ↓
dinode.type == 0
```

---

# 38. balloc() 本质上做什么？

概念：

```text
扫描 bitmap
    │
    ▼
找到 bit = 0
    │
    ▼
设置 bit = 1
    │
    ▼
返回对应 block number
```

简化伪代码：

```c
for(each bitmap block) {
    bp = bread(...);

    for(each bit) {
        if(bit == 0) {
            set bit to 1;
            log_write(bp);
            return blockno;
        }
    }
}
```

---

# 39. bfree() 本质上做什么？

释放 block：

```text
bitmap:

1
↓
0
```

也就是：

```text
used → free
```

文件删除/截断时，`itrunc()` 最终会释放：

```text
直接 data blocks
一级间接 data blocks
indirect block 本身
```

---

# 40. fd、file、inode、block 是四种不同层次

用户拿到：

```text
fd = 3
```

这不是 inode number，也不是 block number。

关系：

```text
Process
│
│ ofile[3]
▼
struct file
│
│ off
│ readable
│ writable
▼
struct inode
│
│ dev
│ inum
│ size
│ addrs[]
▼
filesystem blocks
```

---

# 41. 为什么还需要 struct file？

inode 表示：

```text
文件本身
```

而 `struct file` 表示：

```text
一次打开文件的状态
```

其中非常重要的是：

```c
uint off;
```

即当前读写偏移量。

例如两个独立 open：

```text
fd1 → file A → inode 35
      off=100

fd2 → file B → inode 35
      off=900
```

它们可以指向同一 inode，但拥有不同 offset。

---

# 42. dup() 为什么共享 offset？

`dup(fd)` 不是创建新的 inode，也通常不是创建新的 `struct file`。

而是：

```text
fd1 ─┐
     ├──→ same struct file
fd2 ─┘
             │
             ▼
          inode
```

所以：

```text
fd1 / fd2
共享同一个 file->off
```

---

# 43. nlink 和 ref 不一样

磁盘 dinode：

```c
short nlink;
```

表示：

```text
文件系统中有多少目录项指向这个 inode
```

例如：

```text
a.txt ─┐
       ▼
    inode 35
       ▲
b.txt ─┘
```

则：

```text
nlink = 2
```

而内存 inode 的：

```text
ref
```

表示：

```text
内存中当前有多少引用
```

因此：

```text
nlink = 目录树引用
ref   = 内存运行时引用
```

---

# 44. 为什么 unlink 后打开的文件还能继续读？

例如：

```c
fd = open("a.txt", ...);
unlink("a.txt");
```

目录项被删：

```text
"a.txt" → inode
```

不存在了，所以：

```text
nlink--
```

但 fd 仍然：

```text
fd
 ↓
struct file
 ↓
inode
```

因此 inode 还有内存引用。

真正释放文件通常需要达到：

```text
nlink == 0
AND
ref == 0
```

这体现了：

```text
文件名
≠
文件本体
```

---

# 45. Buffer Cache：block 在内存里的缓存

磁盘 IO 最终不是每次都直接访问硬盘。

xv6 通过：

```text
buffer cache
```

缓存 filesystem block。

核心对象：

```c
struct buf
```

概念：

```text
disk block 100
      │
      ▼
struct buf
      │
      ├── blockno = 100
      └── data[BSIZE]
```

---

# 46. bread / bwrite / brelse

常见接口：

```c
bread(dev, blockno)
bwrite(bp)
brelse(bp)
```

含义：

```text
bread
=
取得某个 block 对应的 buffer
必要时从磁盘读入

bwrite
=
把 buffer 内容写到磁盘

brelse
=
释放当前对 buffer 的引用
不是 free 这个 buffer
```

---

# 47. Buffer Cache 不只是缓存，也负责一致性

buffer cache 要保证：

```text
同一个 (dev, blockno)
尽量对应唯一的内存 buffer 对象
```

否则：

```text
buf A → block 100
buf B → block 100
```

两个不同副本可能被并发修改，造成一致性问题。

所以 buffer cache 同时承担：

```text
cache
+
synchronization
```

---

# 48. Logging / Journal 的作用

一次文件系统操作经常需要修改多个 block。

例如创建文件可能需要：

```text
修改 inode
修改目录 block
修改 bitmap
修改 data block
```

如果写一半系统崩溃，可能导致文件系统不一致。

因此 xv6 使用：

```text
write-ahead logging
```

大致思想：

```text
修改多个 block
    │
    ▼
先记录到 log
    │
    ▼
commit
    │
    ▼
再安装到真正 home location
```

常见接口：

```c
begin_op()
log_write()
end_op()
```

---

# 49. 一次 read() 的底层路径

例如：

```c
read(fd, buf, 100);
```

大致：

```text
sys_read
   │
   ▼
proc->ofile[fd]
   │
   ▼
struct file
   │
   ▼
fileread
   │
   ▼
readi
   │
   ▼
根据 offset 算文件逻辑 block
   │
   ▼
bmap
   │
   ▼
filesystem block number
   │
   ▼
bread
   │
   ▼
buffer cache
   │
   ▼
disk
```

---

# 50. 一个完整的综合例子

假设：

```text
/home/a.txt
```

## 第一步：路径解析

```text
ROOTINO = 1
   │
   ▼
root directory inode
   │
   │ data = dirent[]
   ▼
找到 "home"
   │
   ▼
inum = 20
   │
   ▼
inode 20
   │
   │ data = dirent[]
   ▼
找到 "a.txt"
   │
   ▼
inum = 35
   │
   ▼
inode 35
```

## 第二步：找到 inode 35 在 inode table 的位置

假设：

```text
IPB = 16
sb.inodestart = 32
```

则：

```text
35 / 16 = 2
35 % 16 = 3
```

所以：

```text
inode 35
=
block 34
slot 3
```

## 第三步：读取 inode 35 的文件内容

假设：

```text
inode 35

type = T_FILE
size = 20000
```

读取：

```text
offset = 15000
```

逻辑 block：

```text
15000 / 1024 = 14
```

因为：

```text
14 >= NDIRECT(12)
```

所以走一级间接：

```text
14 - 12 = 2
```

假设：

```text
inode35.addrs[12] = 400
```

则：

```text
block 400
=
indirect block
```

里面：

```text
uint[2] = 850
```

因此真正读取：

```text
disk block 850
```

完整链路：

```text
/home/a.txt
   │
   ▼
directory dirent
   │
   ▼
inum 35
   │
   ▼
inode block 34 / slot 3
   │
   ▼
inode35.addrs[12]
   │
   ▼
indirect block 400
   │
   ▼
uint[2] = 850
   │
   ▼
data block 850
   │
   ▼
文件实际 bytes
```

---

# 51. 最重要的编号不要混

| 名称 | 含义 |
|---|---|
| `fd` | 当前进程 `ofile[]` 的下标 |
| `inum` | inode table 中 inode 的编号 |
| `blockno` | filesystem block 编号 |
| `dev` | inode 所属文件系统设备 |
| `major` | T_DEVICE 对应哪个设备驱动 |
| `minor` | 同一驱动下的具体设备实例 |

例如：

```text
fd = 3
    │
    ▼
struct file
    │
    ▼
inode:
    dev  = 1
    inum = 35
    size = 20000
    addrs[0] = 100
```

这里：

```text
3
1
35
100
```

分别属于完全不同的编号空间。

---

# 52. 最重要的几个公式

## Block 大小

```text
BSIZE = 1024 bytes
```

## dinode 大小

```text
sizeof(dinode) = 64 bytes
```

## 每 block inode 数

```text
IPB
=
BSIZE / sizeof(dinode)
=
1024 / 64
=
16
```

## inode block

```text
IBLOCK(inum)
=
sb.inodestart + inum / 16
```

## inode block 内 slot

```text
slot
=
inum % 16
```

## 一个 bitmap block 管多少 filesystem blocks

```text
BPB
=
BSIZE × 8
=
1024 × 8
=
8192
```

## bitmap block

```text
BBLOCK(blockno)
=
sb.bmapstart + blockno / 8192
```

## bitmap block 内 bit

```text
bit offset
=
blockno % 8192
```

## direct block 数

```text
NDIRECT = 12
```

## indirect block 内地址数

```text
NINDIRECT
=
BSIZE / sizeof(uint)
=
1024 / 4
=
256
```

## 最大文件数据块数

```text
MAXFILE
=
12 + 256
=
268 blocks
```

## 最大文件大小

```text
268 × 1024
=
274432 bytes
≈ 268 KiB
```

---

# 53. 最后一张总图

```text
                    xv6 filesystem
                           │
                           ▼
                     pathname
                           │
                           ▼
                  directory inode
                           │
                    data blocks
                           │
                           ▼
                  struct dirent[]
                    name → inum
                           │
                           ▼
                      inode number
                           │
              ┌────────────┴────────────┐
              │                         │
        inum / IPB                inum % IPB
              │                         │
              ▼                         ▼
         inode block                  slot
              │
              └────────────┬────────────┘
                           ▼
                        dinode
                           │
                ┌──────────┴──────────┐
                │                     │
             type/size              addrs[]
                                      │
                    ┌─────────────────┴─────────────────┐
                    │                                   │
             addrs[0..11]                          addrs[12]
                    │                                   │
                    ▼                                   ▼
              data blocks                        indirect block
                                                        │
                                               uint blockno[]
                                                        │
                                                        ▼
                                                   data blocks
```

如果：

```text
type == T_FILE
```

则 data block：

```text
普通文件 raw bytes
```

如果：

```text
type == T_DIR
```

则 data block：

```text
struct dirent[]
```

而所有 block 的分配状态统一由：

```text
bitmap
```

记录：

```text
1 bit ↔ 1 filesystem block
```

---

# 54. 建议记住的五句话

1. **inode 不存文件名，目录项存 `name → inode number`。**
2. **inode 的 `addrs[]` 存的是磁盘 block number，不是文件内容。**
3. **普通文件 data block 存 raw bytes；目录 data block 存 `struct dirent[]`。**
4. **直接索引是 `inode → data block`；一级间接是 `inode → 地址表 block → data block`。**
5. **bitmap 中一个 bit 对应一个 filesystem block，1 表示已占用，0 表示空闲。**

---

# 55. 推荐阅读 xv6 源码顺序

为了把本文概念和源码对应起来，建议按这个顺序看：

```text
kernel/fs.h
    ↓
struct superblock
struct dinode
struct dirent
NDIRECT / NINDIRECT / IPB / BPB

kernel/fs.c
    ↓
readsb
ialloc
iget
ilock
bmap
readi
writei
dirlookup
dirlink
namex

kernel/bio.c
    ↓
bget
bread
bwrite
brelse

kernel/log.c
    ↓
begin_op
log_write
end_op
commit

kernel/file.c
    ↓
filealloc
fileread
filewrite

kernel/sysfile.c
    ↓
sys_open
sys_read
sys_write
sys_link
sys_unlink
```

如果能顺着：

```text
open("/home/a.txt")
        ↓
namei
        ↓
dirlookup
        ↓
inode
        ↓
read
        ↓
readi
        ↓
bmap
        ↓
bread
        ↓
buffer cache
        ↓
disk
```

完整走一遍源码，xv6 文件系统的底层数据结构基本就串起来了。
