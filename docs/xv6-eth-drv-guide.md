# xv6 E1000 Ethernet Driver Guide

## 1. 驱动整体视角

xv6 中的 E1000 驱动，本质上是在内核网络栈和网卡硬件之间维护两条 DMA 队列：

- TX（Transmit）队列：内核把待发送 packet 交给 E1000。
- RX（Receive）队列：E1000 把收到的 packet DMA 到内核提供的 buffer。

整体路径可以抽象为：

```text
用户进程
   |
socket / UDP / IP / ARP
   |
  net.c
   |
   +---------------- TX ----------------+
   |                                    |
   v                                    |
e1000_transmit()                         |
   |                                    |
TX descriptor ring                      |
   |                                    |
   | DMA read                           |
   v                                    |
 E1000  ---------------- Ethernet ------+
   |
   | DMA write
   v
RX descriptor ring
   |
e1000_recv()
   |
 net_rx()
```

E1000 驱动本身主要关心：

1. MMIO 寄存器。
2. TX/RX descriptor ring。
3. DMA buffer。
4. descriptor ownership。
5. 中断和 completion。

驱动并不需要理解 UDP、IP、ARP 的具体语义；它看到的主要是：

```text
buffer address + packet length
```

---

# 2. MMIO：CPU 如何控制 E1000

PCI 初始化阶段找到 E1000 后，会把设备 BAR 映射到 CPU 可访问的地址空间。

xv6 中通常保存：

```c
static volatile uint32 *regs;
```

之后：

```c
regs[E1000_TDT] = 5;
```

并不是普通数组写入，而是在执行：

```text
CPU store
   |
   v
MMIO address
   |
   v
E1000 hardware register
```

常见寄存器包括：

```text
TX:
TDBAL/TDBAH  TX descriptor ring base
TDLEN        TX ring length
TDH          TX descriptor head
TDT          TX descriptor tail

RX:
RDBAL/RDBAH  RX descriptor ring base
RDLEN        RX ring length
RDH          RX descriptor head
RDT          RX descriptor tail

Interrupt:
IMS          interrupt mask set
IMC          interrupt mask clear
ICR          interrupt cause read

Control:
TCTL
RCTL
TIPG
...
```

---

# 3. DMA：为什么网卡可以直接访问内存

E1000 使用 DMA。

发送时：

```text
kernel mbuf
+------------------+
| Ethernet packet  |
+------------------+
       |
       | physical/DMA address
       v
TX descriptor
       |
       v
E1000 DMA read
       |
       v
Ethernet
```

接收时：

```text
Ethernet
   |
   v
E1000
   |
   | DMA write
   v
kernel RX mbuf
```

CPU 不需要逐字节把 packet 搬进设备寄存器。

CPU 主要负责：

```text
告诉网卡：
buffer 在哪里
buffer 多长
descriptor 是否可用
```

---

# 4. Descriptor 是 CPU 与设备之间的硬件协议

TX/RX descriptor 的字段布局由 E1000 硬件规范规定。

硬件并不知道 C 语言里的：

```c
struct tx_desc
```

它只知道 descriptor 内：

```text
offset X -> buffer address
offset Y -> length
offset Z -> cmd/status
```

所以 xv6 的 C struct 必须严格匹配 Intel E1000 定义的 layout。

典型 TX descriptor 关心：

```text
addr
length
cmd
status
```

典型 RX descriptor 关心：

```text
addr
length
status
errors
```

---

# 5. Descriptor Ring

TX/RX descriptor 都组织为循环数组：

```c
struct tx_desc tx_ring[TX_RING_SIZE];
struct rx_desc rx_ring[RX_RING_SIZE];
```

例如 ring size = 8：

```text
0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7
^                                  |
|__________________________________|
```

所以索引永远是：

```c
next = (index + 1) % RING_SIZE;
```

例如：

```text
6 -> 7 -> 0 -> 1
```

是完全正常的。

---

# 6. TX Ring：TDH / TDT

TX 最重要的规则：

```text
TDH：hardware 推进
TDT：software 推进
```

也就是：

```text
software producer -> TDT
hardware consumer -> TDH
```

---

## 6.1 TDT 的语义

`TDT` 指向：

> software 下一次要填写的 TX descriptor，也可以理解为当前已提交区域的尾后位置。

例如：

```text
TDH = 2
TDT = 5
```

则 hardware 当前需要处理：

```text
2 -> 3 -> 4
```

即：

```text
[TDH, TDT)
```

而不是包括 descriptor 5。

---

## 6.2 软件如何提交 TX descriptor

假设：

```text
TDT = 5
```

software：

```c
tx_ring[5].addr = ...;
tx_ring[5].length = ...;
tx_ring[5].cmd = ...;
tx_ring[5].status = 0;
```

然后：

```c
regs[E1000_TDT] = 6;
```

这一步可以理解为：

```text
descriptor 5 构造完成
       |
memory barrier
       |
TDT: 5 -> 6
       |
       v
descriptor 5 ownership 交给 hardware
```

更新 TDT 就像给设备按了一次 doorbell。

---

## 6.3 TDH 的语义

硬件从 TDH 开始消费 descriptor。

例如：

```text
TDH = 2
TDT = 6
```

处理顺序：

```text
2 -> 3 -> 4 -> 5
```

处理完后：

```text
TDH = 6
TDT = 6
```

所以：

```text
TDH == TDT
```

表示：

> 当前 TX queue 没有等待 hardware 发送的 descriptor。

---

## 6.4 TX queue empty 与 packet completion 不同

要严格区分：

```text
TDH == TDT
```

表示：

> 整个当前 TX queue drain 完。

而：

```text
某个 packet 的最后一个 descriptor DD = 1
```

表示：

> 这个 packet 已经完成，可以回收其 buffer。

普通 packet completion 不应该强制等待整个 TX queue 空。

---

# 7. TX Descriptor Ownership

TX 生命周期：

```text
DD = 1
software owns descriptor
       |
       | 填 addr/length/cmd
       | status = 0
       v
推进 TDT
       |
       v
hardware owns descriptor + DMA buffer
       |
       | DMA read
       | transmit
       v
hardware write back DD = 1
       |
       v
software owns descriptor again
```

因此：

> 提交 TX descriptor 后不能立即 `mbuffree(m)`。

否则 E1000 可能还在 DMA 读取该 mbuf。

---

# 8. `tx_mbufs[]` 的作用

descriptor 里只记录 DMA 地址：

```c
tx_ring[i].addr = (uint64)m->head;
```

但 software 还需要知道：

> 这个 DMA 地址属于哪个 `struct mbuf`？

所以 xv6 通常维护：

```c
static struct mbuf *tx_mbufs[TX_RING_SIZE];
```

关系：

```text
tx_ring[i]
   |
   | addr
   v
packet memory

tx_mbufs[i]
   |
   v
struct mbuf
```

当：

```text
tx_ring[i].status & DD
```

成立时，才可以：

```c
mbuffree(tx_mbufs[i]);
```

---

# 9. Scatter/Gather DMA

如果一个逻辑 packet 分散在多个内存 buffer：

```text
m0 -> m1 -> m2
```

E1000 可以通过多个 descriptor 描述同一个 packet：

```text
desc[5] -> m0
desc[6] -> m1
desc[7] -> m2
```

这就是：

```text
scatter/gather DMA
```

hardware 可以分别 DMA 多块不连续内存，最后组合成一个 Ethernet frame。

---

# 10. EOP：定义 packet 边界

对于：

```text
m0 -> m1 -> m2
```

属于同一个 packet：

```text
desc[5] cmd: EOP = 0
desc[6] cmd: EOP = 0
desc[7] cmd: EOP = 1
```

只有最后一个 descriptor 设置：

```text
EOP = End Of Packet
```

硬件看到：

```text
desc5 -> desc6 -> desc7(EOP)
```

才知道一个完整 packet 到此结束。

非常重要：

> `EOP` 是网络 packet 的边界，不是“这一批 descriptor 提交结束”的标志。

因此如果一个 packet 需要 9 个 descriptor，但当前只有 8 个可用，不能人为：

```text
前 8 个 -> EOP
第 9 个 -> EOP
```

否则硬件会发送成两个 Ethernet frame。

---

# 11. RS 与 DD：packet completion

`RS`：

```text
Report Status
```

表示：

> 这个 descriptor 完成后，请 hardware write back status。

对于多 descriptor packet：

```text
desc5 -> m0
desc6 -> m1
desc7 -> m2
```

常见做法：

```text
desc5: EOP=0 RS=0
desc6: EOP=0 RS=0
desc7: EOP=1 RS=1
```

即：

```text
desc5 -> desc6 -> desc7(EOP|RS)
```

hardware 完成最后 descriptor 后：

```text
desc7.status.DD = 1
```

因此：

```text
final descriptor DD = 1
```

可以作为：

> 整个 packet DMA 生命周期结束

的 completion 标志。

---

# 12. 为什么不能因为 ring 空间不足拆 packet

假设：

```text
packet = 9 个 mbuf
当前 free TX descriptor = 8
```

有两种情况。

---

## 12.1 ring 总容量 > 9，只是当前空闲 8

例如：

```text
TX_RING_SIZE = 64
free = 8
need = 9
```

应该：

```text
等待 TX completion
      |
      v
reclaim descriptors
      |
      v
free >= 9
      |
      v
一次提交整个 packet
```

而不是拆成两个 EOP。

---

## 12.2 整个 ring 最大只有 8

例如：

```text
TX_RING_SIZE = 8
need = 9
```

则这个 packet 永远不能直接用 9 个 descriptor 提交。

解决方法：

1. 增大 TX ring。
2. linearize / coalesce mbuf。
3. 使用更大的连续 packet buffer。

例如：

```text
9 个 mbuf
   |
linearize
   v
1 个连续 mbuf
   |
1 descriptor
```

---

# 13. 为什么 xv6 可以简化成一个 packet 一个 mbuf

普通 Ethernet MTU 通常是：

```text
IP MTU              1500 bytes
Ethernet header       14 bytes
----------------------------
frame                1514 bytes
```

即使加 VLAN tag，也仍远小于 2048 bytes。

所以 xv6 可以采用简单假设：

```text
1 Ethernet packet
       =
1 x 2048-byte mbuf
       =
1 TX descriptor
       =
1 RX descriptor
```

这极大简化驱动。

Jumbo Frame（例如 MTU 9000）才需要更大的 buffer 或多个 descriptor。

---

# 14. 简化后的 TX：每个 descriptor 都是 EOP | RS

在这个假设下，不再需要 scatter/gather TX。

发送：

```text
mbuf
 |
 v
TX descriptor
```

所以每个 TX descriptor 都表示完整 packet：

```c
desc->addr = (uint64)m->head;
desc->length = m->len;
desc->cmd =
    E1000_TXD_CMD_EOP |
    E1000_TXD_CMD_RS;
desc->status = 0;
```

然后：

```c
tx_mbufs[index] = m;

__sync_synchronize();
regs[E1000_TDT] =
    (index + 1) % TX_RING_SIZE;
```

完整流程：

```text
net_tx(m)
   |
   v
e1000_transmit(m)
   |
   v
TDT = index
   |
   v
检查 DD == 1
   |
   v
desc.addr = m->head
desc.len  = m->len
desc.cmd  = EOP | RS
desc.DD   = 0
   |
   v
tx_mbufs[index] = m
   |
memory barrier
   |
   v
TDT++
   |
   v
hardware DMA + transmit
```

---

# 15. TXDW：发送完成中断

如果 descriptor 设置：

```text
RS = 1
```

hardware 完成 descriptor 后会 write back：

```text
DD = 1
```

如果驱动启用：

```text
TXDW
Transmit Descriptor Written Back
```

则可以触发 TX completion interrupt。

初始化可概念化为：

```c
regs[E1000_IMS] =
    E1000_INT_TXDW |
    E1000_INT_RXDW;
```

于是：

```text
TX descriptor EOP|RS
       |
hardware transmit
       |
DD = 1
       |
TXDW
       |
E1000 interrupt
       |
e1000_intr()
       |
tx reclaim
```

---

# 16. TXDW 不表示整个 TX ring 已完成

必须区分：

```text
TXDW
```

和：

```text
TDH == TDT
```

TXDW 表示：

> 至少有要求 RS 的 TX descriptor 完成 write-back。

它不保证：

```text
TDH == TDT
```

例如：

```text
packet A -> desc0 EOP|RS
packet B -> desc1 EOP|RS
packet C -> desc2 EOP|RS
```

hardware 完成 A：

```text
desc0.DD = 1
```

即可产生 TXDW。

这时 B/C 仍可能 pending。

---

# 17. TX Completion Reclaim

在简化的一 packet 一 descriptor 模型中，TX reclaim 非常简单：

```c
static void
e1000_tx_reclaim(void)
{
  acquire(&e1000_lock);

  for(int i = 0; i < TX_RING_SIZE; i++){
    if(tx_mbufs[i] &&
       (tx_ring[i].status & E1000_TXD_STAT_DD)){
      struct mbuf *m = tx_mbufs[i];

      tx_mbufs[i] = 0;
      mbuffree(m);
    }
  }

  release(&e1000_lock);
}
```

状态可以理解为：

```text
tx_mbufs[i] == NULL
    没有未回收 packet

tx_mbufs[i] != NULL && DD == 0
    hardware 正在使用该 packet

tx_mbufs[i] != NULL && DD == 1
    hardware 已完成，software 可以 reclaim
```

---

# 18. TX lazy reclaim

原始 xv6 风格也可以不使用 TX interrupt。

下一次发送重新绕到某 descriptor 时：

```c
if(desc->status & DD){
    if(tx_mbufs[index])
        mbuffree(tx_mbufs[index]);
}
```

这叫：

```text
lazy reclaim
```

特点：

```text
packet 已发送完成
       |
buffer 暂不释放
       |
等 TDT 将来绕回该位置
       |
下一次 transmit 顺手回收
```

这是安全的，只是 buffer 回收不及时。

如果启用了：

```text
EOP | RS + TXDW
```

则更自然的做法是在中断里立即 reclaim。

---

# 19. RX Ring：RDH / RDT

RX 是最容易混淆的部分。

规则：

```text
RDH：hardware 推进
RDT：software 推进
```

但是 RX 和 TX 的 producer/consumer 角色相反：

```text
RX:
hardware producer
software consumer
```

可以记成：

```text
hardware 在 RDH 位置生产“收到的 packet”
software 从 RDT+1 开始消费 completed packet
```

---

# 20. RX 初始化

xv6 先给每个 descriptor 一个空 mbuf：

```c
for(i = 0; i < RX_RING_SIZE; i++){
    rx_mbufs[i] = mbufalloc(0);
    rx_ring[i].addr =
        (uint64)rx_mbufs[i]->head;
}
```

然后：

```c
regs[E1000_RDH] = 0;
regs[E1000_RDT] = RX_RING_SIZE - 1;
```

假设 ring size = 8：

```text
RDH = 0
RDT = 7
```

硬件下一次从：

```text
rx_ring[0]
```

开始接收。

software 下一次检查：

```c
(RDT + 1) % 8
```

同样得到：

```text
0
```

---

# 21. RDH 的语义

`RDH` 表示：

> hardware 下一块准备用于接收 packet 的 descriptor。

例如：

```text
RDH = 2
```

收到一个 packet 后：

```text
Ethernet packet
       |
       v
rx_ring[2].addr
       |
       v
mbuf
```

hardware DMA 完成后：

```text
rx_ring[2].length = ...
rx_ring[2].status.DD = 1
```

然后：

```text
RDH = 3
```

所以：

```text
RDH = hardware producer pointer
```

---

# 22. RDT 的语义

`RDT` 是 software 管理的 RX buffer availability / reclaim 边界。

xv6 软件每次从：

```c
index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
```

开始检查。

例如：

```text
RDT = 7
```

则检查：

```text
rx[0]
```

如果：

```text
rx[0].DD = 1
```

software 取出 packet。

然后给 rx[0] 换一个新的空 mbuf，清 status，并：

```c
regs[E1000_RDT] = 0;
```

这一步表示：

> descriptor 0 已被软件处理、重新装上 buffer，并重新交还给 hardware。

---

# 23. RX 接收三个 packet 的例子

初始：

```text
RDH = 0
RDT = 7
```

hardware 连续收到 A/B/C：

```text
rx[0] <- A, DD=1
RDH=1

rx[1] <- B, DD=1
RDH=2

rx[2] <- C, DD=1
RDH=3
```

状态：

```text
      A     B     C
      ↓     ↓     ↓
+-----+-----+-----+-----+-----+-----+-----+-----+
|DD=1 |DD=1 |DD=1 |     |     |     |     |     |
+-----+-----+-----+-----+-----+-----+-----+-----+
                  ↑                             ↑
                 RDH                           RDT
```

software：

```text
RDT=7 -> 检查 0 -> consume A -> RDT=0
RDT=0 -> 检查 1 -> consume B -> RDT=1
RDT=1 -> 检查 2 -> consume C -> RDT=2
RDT=2 -> 检查 3 -> DD=0 -> stop
```

最终：

```text
RDT = 2
RDH = 3
```

没有待 software 处理的 packet。

---

# 24. RX Descriptor Ownership

RX 生命周期：

```text
software 准备 empty mbuf
       |
       v
desc.addr = mbuf->head
status = 0
RDT 推进
       |
       v
hardware owns RX descriptor/buffer
       |
       | packet arrives
       | DMA write
       v
length
EOP
DD = 1
       |
       v
software owns received packet
       |
       | 取 old mbuf
       | 放 replacement mbuf
       | status = 0
       v
推进 RDT
       |
       v
hardware owns descriptor again
```

---

# 25. RX 中断不是“head 到 tail 全处理完”的中断

这是非常关键的一点。

RX interrupt 不表示：

```text
hardware 已从 RDH 一直跑到 RDT
并把整个 ring 全处理完
```

它更接近：

> 有 RX descriptor 完成了，需要 software 来检查 ring。

因此一次 interrupt 可能对应：

```text
1 个 packet
```

也可能 CPU 真正进入 handler 时已经积累了：

```text
多个 packet
```

例如：

```text
packet A -> rx[0].DD=1 -> interrupt

CPU 尚未进入 handler

packet B -> rx[1].DD=1
packet C -> rx[2].DD=1

CPU:
e1000_recv()
  -> process rx[0]
  -> process rx[1]
  -> process rx[2]
  -> rx[3].DD == 0
  -> return
```

所以 `e1000_recv()` 必须循环处理：

```c
for(;;){
    index = (RDT + 1) % N;

    if(!(rx[index].status & DD))
        return;

    ...
}
```

---

# 26. 为什么 RX 依赖 DD，而不是直接看 RDH

虽然 RDH 反映 hardware 的进度，但 descriptor 的：

```text
DD
```

才是明确的 completion / ownership 标志。

software 应该判断：

```c
desc->status & E1000_RXD_STAT_DD
```

而不是简单：

```text
只要 index != RDH 就认为 packet 完成
```

可以把 DD 理解为：

> hardware 明确告诉 software：这个 descriptor 的 packet 数据和 metadata 已经完成 write-back，可以消费。

---

# 27. RX 的单 mbuf 简化

因为一个普通 Ethernet frame 小于 2048 bytes，所以可以要求：

```text
DD = 1
EOP = 1
errors = 0
length <= MBUF_SIZE
```

才交给 `net_rx()`。

例如：

```c
if((desc->status & E1000_RXD_STAT_EOP) &&
   desc->errors == 0 &&
   desc->length <= MBUF_SIZE){
    ...
}
```

如果：

```text
DD = 1
EOP = 0
```

说明一个 descriptor 没有装下完整 packet，与当前简单模型不符，可以直接 drop。

---

# 28. RX replacement buffer

收到 packet 后，原来的 mbuf 要交给网络栈：

```text
old RX mbuf
    |
    v
net_rx(m)
```

所以必须给 descriptor 换新的 buffer：

```c
replacement = mbufalloc(0);

rx_mbufs[index] = replacement;
desc->addr = (uint64)replacement->head;
```

然后：

```c
desc->status = 0;
regs[E1000_RDT] = index;
```

完整 ownership：

```text
old mbuf
   |
   +--> network stack

replacement mbuf
   |
   +--> RX descriptor
          |
          +--> E1000
```

如果 replacement allocation 失败，可以 drop 当前 packet，并复用原 buffer，而不是 panic。

---

# 29. `net_rx()` 应该在 E1000 锁外调用

这一点对 xv6 很重要。

可能出现：

```text
E1000 RX
   |
e1000_recv()
   |
net_rx()
   |
ARP request
   |
生成 ARP reply
   |
net_tx()
   |
e1000_transmit()
```

如果 `e1000_recv()` 持有：

```text
e1000_lock
```

调用 `net_rx()`，而 `e1000_transmit()` 也需要同一把锁，就会：

```text
deadlock
```

所以正确结构：

```text
lock
  |
处理 RX descriptor
换 replacement
清 status
推进 RDT
  |
unlock
  |
net_rx(old_mbuf)
```

---

# 30. TX/RX Head/Tail 对照表

最值得记住的是这张表：

| Queue | Head | Tail | Producer | Consumer |
|---|---|---|---|---|
| TX | `TDH`，hardware 推进 | `TDT`，software 推进 | software | hardware |
| RX | `RDH`，hardware 推进 | `RDT`，software 推进 | hardware | software |

进一步：

```text
TX:
software 填 descriptor
    |
    v
推进 TDT

hardware 从 TDH 消费
    |
    v
推进 TDH


RX:
hardware 从 RDH 写 packet
    |
    v
推进 RDH

software 从 RDT+1 消费
    |
    v
重新装 buffer
    |
    v
推进 RDT
```

---

# 31. TX 与 RX 的最终统一理解：Ownership Queue

TX：

```text
software
   |
   | create work
   v
descriptor
   |
   | TDT
   v
hardware
   |
   | DMA + transmit
   v
DD
   |
   v
software reclaim
```

RX：

```text
software
   |
   | provide empty buffer
   v
descriptor
   |
   | RDT
   v
hardware
   |
   | DMA received packet
   v
DD
   |
   v
software consume
```

所以 head/tail、DD、interrupt 的本质都围绕：

> descriptor 和 DMA buffer 当前归谁所有？

---

# 32. 简化版 xv6 E1000 TX 完整路径

采用：

```text
1 packet = 1 mbuf = 1 TX descriptor
```

后：

```text
net_tx(m)
   |
   v
e1000_transmit(m)
   |
   v
index = TDT
   |
   v
检查 desc[index].DD == 1
   |
   v
desc.addr = m->head
desc.length = m->len
desc.cmd = EOP | RS
desc.status = 0
tx_mbufs[index] = m
   |
memory barrier
   |
   v
TDT = next
   |
   v
E1000 DMA read
   |
   v
Ethernet transmit
   |
   v
DD = 1
   |
   v
TXDW interrupt
   |
   v
e1000_tx_reclaim()
   |
   v
mbuffree(m)
```

---

# 33. 简化版 xv6 E1000 RX 完整路径

```text
rx_ring[i].addr
      |
      v
empty 2048B mbuf
      |
      v
E1000 owns buffer
      |
packet arrives
      |
DMA write
      |
length / EOP / DD
      |
RXDW interrupt
      |
e1000_recv()
      |
index = RDT + 1
      |
检查 DD
      |
取出 old mbuf
      |
分配 replacement
      |
replacement 写回 desc.addr
status = 0
RDT = index
      |
unlock
      |
net_rx(old mbuf)
```

---

# 34. 最后记忆模型

如果只保留几个关键结论：

## TX

```text
TDT = software producer pointer
TDH = hardware consumer pointer

software:
fill descriptor
-> EOP|RS
-> clear DD
-> TDT++

hardware:
TDH 开始消费
-> DMA
-> transmit
-> DD
-> TXDW
```

## RX

```text
RDH = hardware producer pointer
RDT = software consumer/rearm boundary

hardware:
从 RDH 收 packet
-> DMA
-> DD/EOP
-> RDH++

software:
从 RDT+1 检查 DD
-> consume
-> replacement buffer
-> clear status
-> RDT++
```

## Scatter/Gather

```text
多个 buffer
-> 多个 descriptor
-> 只有最后一个 descriptor 设置 EOP
```

## xv6 简化假设

```text
普通 Ethernet MTU 1500
        <
2048-byte mbuf

因此：

1 packet
= 1 mbuf
= 1 TX/RX descriptor

TX:
每个 descriptor 直接 EOP | RS
```

## 最重要的一句话

> E1000 descriptor ring 的核心不是“数组怎么走”，而是用 `TDH/TDT/RDH/RDT + DD/EOP/RS` 在 software 和 hardware 之间明确表达 **queue 边界、packet 边界和 DMA buffer ownership**。
