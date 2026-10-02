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




这个 E1000 驱动的架构模型可以概括为：

  协议栈 / socket
     |
     | mbuf
     v
  net.c: Ethernet/IP/ARP/UDP
     |
     | e1000_transmit() / net_rx()
     v
  e1000.c 驱动
     |
     | MMIO 控制寄存器 + DMA 描述符环
     v
  QEMU E1000 设备
     |
     | PLIC 外部中断 IRQ 33
     v
  trap.c -> e1000_intr()

  1. 设备访问模型：MMIO + DMA

  这个驱动不是用普通 I/O 指令访问网卡，而是使用 MMIO 寄存器 控制设备。

  E1000 的 MMIO 基址固定配置为：

  #define E1000_MMIO 0x40000000L

  见 kernel/memlayout.h:32。

  内核页表把它恒等映射：

  kvmmap(kpgtbl, E1000_MMIO, E1000_MMIO,
         E1000_MMIO_SIZE, PTE_R | PTE_W);

  见 kernel/vm.c:42。

  PCI 初始化时，会把 E1000 的 BAR0 配到这个地址，并开启：

  PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER

  见 kernel/pci.c:131。

  其中 PCI_COMMAND_MASTER 很关键：它允许 E1000 作为 bus master 发起 DMA，直接读写内存中的描述符和 mbuf 数据区。

  所以整体是：

  CPU 通过 MMIO 写寄存器告诉网卡 ring 在哪里
  网卡通过 DMA 直接读写内存中的 tx_ring / rx_ring / mbuf
  网卡完成后通过中断通知 CPU

  2. 寄存器定义模型

  寄存器定义在 kernel/e1000_dev.h:4。

  因为驱动把 MMIO 基址当成：

  volatile uint32 *regs;

  所以手册里的字节偏移要除以 4：

  #define E1000_TDBAL (0x03800 / 4)
  #define E1000_TDT   (0x03818 / 4)
  #define E1000_RDBAL (0x02800 / 4)
  #define E1000_RDT   (0x02818 / 4)

  典型寄存器分三类：

  TX 发送环：
    TDBAL/TDBAH   发送描述符环基址
    TDLEN         发送环长度
    TDH           硬件 head
    TDT           软件 tail

  RX 接收环：
    RDBAL/RDBAH   接收描述符环基址
    RDLEN         接收环长度
    RDH           硬件 head
    RDT           软件 tail

  Interrupt：
    IMS           开启中断
    IMC           屏蔽中断
    ICR           读取并清除中断原因

  3. 核心数据结构：两个 DMA 描述符环

  驱动里有两个固定大小的 ring：

  #define TX_RING_SIZE 16
  #define RX_RING_SIZE 16

  static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
  static struct mbuf *tx_mbufs[TX_RING_SIZE];

  static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
  static struct mbuf *rx_mbufs[RX_RING_SIZE];

  见 kernel/e1000.c:12。

  描述符本身在 kernel/e1000_dev.h:50：

  struct tx_desc {
    uint64 addr;
    uint16 length;
    uint8 cmd;
    uint8 status;
    ...
  };

  接收描述符：

  struct rx_desc {
    uint64 addr;
    uint16 length;
    uint8 status;
    uint8 errors;
    ...
  };

  addr 是设备 DMA 要读写的报文缓冲区地址。

  因为 xv6 内核使用 direct map，内核虚拟地址和物理地址相同，所以这里直接把 tx_ring、rx_ring、mbuf->head 地址交给设备使用。

  4. mbuf 是驱动和协议栈之间的包对象

  mbuf 在网络栈中表示一个完整报文缓冲区，分配在 kernel/net.c:55：

  struct mbuf *m = kalloc();
  m->head = m->buf + headroom;
  m->len = 0;

  发送方向：

  socket / UDP / IP / Ethernet
    -> mbufpush() 逐层压入协议头
    -> eth_tx()
    -> e1000_transmit()
    -> E1000 DMA 读取 mbuf->head

  eth_tx() 最后直接把 mbuf 所有权交给 E1000 驱动，见 kernel/net.c:142。

  接收方向：

  E1000 DMA 写入 rx_mbufs[i]->head
    -> e1000_recv()
    -> net_rx(m)
    -> Ethernet 分发到 ARP 或 IP

  net_rx() 见 kernel/net.c:453。

  5. 初始化流程

  E1000 初始化在 kernel/e1000.c:47。

  主要步骤：

  1. 保存 MMIO 基址：

  regs = xregs;

  2. 屏蔽中断并复位设备：

  regs[E1000_IMC] = 0xffffffff;
  regs[E1000_CTL] |= E1000_CTL_RST;

  3. 初始化 TX ring：

  for (int i = 0; i < TX_RING_SIZE; i++)
    tx_ring[i].status = E1000_TXD_STAT_DD;

  DD 表示 descriptor done。初始化为 DD，表示这些发送描述符当前都空闲，软件可以使用。

  然后把 ring 地址写给设备：

  regs[E1000_TDBAL] = (uint64)tx_ring;
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = 0;
  regs[E1000_TDT] = 0;

  4. 初始化 RX ring：

  rx_mbufs[i] = mbufalloc(0);
  rx_ring[i].addr = (uint64)rx_mbufs[i]->head;

  接收环必须提前给每个描述符准备一个 DMA 缓冲区。否则网卡收到包时没有地方写。

  5. 设置 MAC 地址：

  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x80005634;

  对应 QEMU 默认 MAC：

  52:54:00:12:34:56

  6. 启用发送、接收和中断：

  regs[E1000_TCTL] = ...
  regs[E1000_RCTL] = ...
  regs[E1000_IMS] = E1000_INT_TXDW | E1000_INT_RXDW;

  6. 发送路径

  发送入口是 kernel/e1000.c:103：

  int e1000_transmit(struct mbuf *m)

  流程：

  1. 加 e1000_lock
  2. 读取 TDT，找到下一个 TX 描述符
  3. 检查 DD 位
     - DD=1：描述符空闲
     - DD=0：硬件还没发送完，ring 满，返回 -1
  4. 如果该槽位有旧 mbuf，释放
  5. 填 tx_desc：
     addr   = m->head
     length = m->len
     cmd    = EOP | RS
     status = 0
  6. tx_mbufs[index] = m
  7. memory barrier
  8. 更新 TDT，把描述符交给硬件
  9. 解锁

  关键代码：

  int index = regs[E1000_TDT];
  struct tx_desc *desc = &tx_ring[index];

  if ((desc->status & E1000_TXD_STAT_DD) == 0)
    return -1;

  desc->addr = (uint64)m->head;
  desc->length = m->len;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  desc->status = 0;

  tx_mbufs[index] = m;
  __sync_synchronize();
  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;

  EOP 表示这是一个完整包的结束。这个驱动简化为一个包只用一个描述符。

  RS 表示要求硬件发送完成后回写 DD 状态位。

  7. 发送完成回收

  发送完成后，网卡产生 TXDW 中断。

  中断处理里调用：

  e1000_reclaim();

  见 kernel/e1000.c:134。

  它扫描 TX ring：

  if (tx_mbufs[i] && (tx_ring[i].status & E1000_TXD_STAT_DD)) {
    mbuffree(tx_mbufs[i]);
    tx_mbufs[i] = 0;
  }

  也就是：

  硬件设置 DD
    -> 软件知道 DMA 已经不再使用这个 mbuf
    -> 可以释放 mbuf

  8. 接收路径

  接收处理在 kernel/e1000.c:148。

  核心规则：

  RDT 指向软件最后归还给硬件的描述符
  软件从 RDT + 1 开始看有没有新包

  代码：

  int index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  struct rx_desc *desc = &rx_ring[index];

  if ((desc->status & E1000_RXD_STAT_DD) == 0)
    return;

  如果 DD=1，说明硬件已经 DMA 写入了一个包。

  然后检查：

  desc->status & E1000_RXD_STAT_EOP
  desc->errors == 0
  desc->length <= MBUF_SIZE

  这个驱动要求一个完整包必须放在一个描述符中，不处理跨多个描述符的大包。

  成功接收时：

  1. 取出当前 rx_mbufs[index]，准备上送协议栈
  2. 分配 replacement mbuf
  3. 把 replacement 放回 rx_ring[index]
  4. 清空描述符状态
  5. 更新 RDT，把描述符还给硬件
  6. 释放 e1000_lock
  7. 调用 net_rx(m)

  关键点：调用 net_rx(m) 前必须释放 e1000_lock。

  代码注释也写了原因：

  // net_rx() 可能立即发送 ARP/ICMP 应答，因此绝不能持有 e1000_lock。

  见 kernel/e1000.c:194。

  否则可能出现：

  e1000_recv() 持有 e1000_lock
    -> net_rx()
      -> arp_rx / icmp_rx
        -> eth_tx()
          -> e1000_transmit()
            -> 再次 acquire e1000_lock

  这会自锁死。

  9. 中断模型

  E1000 使用 PLIC 外部中断，IRQ 是：

  #define E1000_IRQ 33

  见 kernel/memlayout.h:34。

  中断分发在 kernel/trap.c:191：

  int irq = plic_claim();

  if (irq == E1000_IRQ) {
    e1000_intr();
  }

  plic_complete(irq);

  e1000_intr() 读取 ICR：

  uint32 cause = regs[E1000_ICR];

  ICR 是 read-clear 寄存器，读出来同时清除中断原因。

  然后：

  if (cause & E1000_INT_TXDW)
    e1000_reclaim();

  if (cause & E1000_INT_RXDW)
    e1000_recv();

  见 kernel/e1000.c:201。

  10. 所有权模型

  这个驱动最重要的是 mbuf 所有权。

  发送：

  协议栈持有 mbuf
    -> e1000_transmit(m) 成功
    -> mbuf 所有权交给驱动/硬件
    -> 硬件 DMA 读取
    -> 硬件置 DD
    -> e1000_reclaim() 释放 mbuf

  如果 e1000_transmit() 返回 -1，说明发送 ring 满，调用者仍然持有 mbuf，需要自己处理或释放。

  接收：

  驱动预先持有 rx_mbufs[i]
    -> 硬件 DMA 写入 rx_mbufs[i]
    -> e1000_recv() 取出该 mbuf
    -> 分配 replacement 补回 ring
    -> 原 mbuf 交给 net_rx()
    -> 协议栈最终消费或释放

  11. 锁模型

  只有一把：

  static struct spinlock e1000_lock;

  保护：

  tx_ring
  tx_mbufs
  rx_ring
  rx_mbufs
  TDT/RDT 操作

  但驱动刻意不在持锁状态下调用协议栈：

  release(&e1000_lock);
  if (deliver)
    net_rx(m);

  这是为了避免协议栈反向调用发送路径造成死锁。

  12. 这个驱动的简化点

  这个 E1000 驱动是教学型实现，做了很多简化：

  1. TX/RX ring 只有 16 个描述符。
  2. 一个包只使用一个描述符，不支持 scatter-gather。
  3. RX 不处理跨多个描述符的大包。
  4. 不使用 checksum offload。
  5. 不实现 NAPI/polling。
  6. 不做复杂 PCI 资源分配，只固定配置 BAR0。
  7. 不支持 MSI/MSI-X，只走 PLIC legacy interrupt。
  8. TX ring 满时直接返回 -1，不阻塞等待。

  一句话总结：这个 E1000 驱动是一个 MMIO 控制 + DMA ring 收发 + PLIC 中断通知 + mbuf 所有权转移 的模型。CPU 不搬运大块网络数据，只负责设置描述符、推进 tail、处理中断；真正的报文
  数据移动由 E1000 设备通过 DMA 完成。

─ Worked for 1m 30s ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────


› 说说  TX 发送环：和RX接收环：的tail和head、软件和硬件是如何配合的？


• E1000 的 TX/RX 都是“环形队列”，但 TX 和 RX 的 head/tail 语义方向不完全一样。可以先记住一句话：

  TX：软件生产描述符，硬件消费描述符。
  RX：硬件生产描述符，软件消费描述符。

  ———

  TX 发送环

  TX ring 是发送队列：

  软件把要发送的包放进描述符
  硬件从描述符里 DMA 读取包并发送出去

  寄存器：

  TDH = Transmit Descriptor Head  硬件消费位置
  TDT = Transmit Descriptor Tail  软件生产位置

  在 xv6 里，软件主要操作 TDT：

  int index = regs[E1000_TDT];
  struct tx_desc *desc = &tx_ring[index];

  见 kernel/e1000.c:106。

  发送前检查这个描述符是否空闲：

  if ((desc->status & E1000_TXD_STAT_DD) == 0)
    return -1;

  DD = Descriptor Done。

  含义：

  DD = 1  硬件已经处理完这个描述符，软件可以重用
  DD = 0  描述符还属于硬件，软件不能覆盖

  发送流程：

  TX ring 初始：
    所有 desc.status = DD
    TDH = 0
    TDT = 0

  软件发送一个包：
    1. 读取 TDT，得到 index
    2. 检查 tx_ring[index].DD 是否为 1
    3. 填 desc.addr / desc.length / desc.cmd
    4. 清 desc.status = 0
    5. tx_mbufs[index] = m
    6. TDT = index + 1
       这一步把描述符交给硬件

  硬件看到 TDT 推进：
    1. 从 TDH 开始消费描述符
    2. DMA 读取 desc.addr 指向的包数据
    3. 发送到网卡
    4. 发送完成后写回 desc.status.DD = 1
    5. 推进 TDH
    6. 触发 TXDW 中断

  可以画成：

  TX ring：软件生产，硬件消费

          软件填这里
             |
             v
  +----+----+----+----+----+
  | D0 | D1 | D2 | D3 | D4 |
  +----+----+----+----+----+
         ^         ^
         |         |
        TDH       TDT

  TDH -> 硬件下一次要处理的位置
  TDT -> 软件下一次要填的位置

  但 xv6 驱动不直接依赖 TDH 判断空闲，而是依赖描述符里的 DD 位。

  核心代码：

  desc->addr = (uint64)m->head;
  desc->length = m->len;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  desc->status = 0;
  tx_mbufs[index] = m;
  __sync_synchronize();
  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;

  见 kernel/e1000.c:117。

  注意顺序：

  先填描述符
  再 memory barrier
  最后更新 TDT

  因为 TDT 一更新，硬件就可能立刻读取描述符。

  ———

  RX 接收环

  RX ring 是接收队列：

  软件提前给硬件准备空 buffer
  硬件收到包后 DMA 写入 buffer
  软件再取走已经写好的包

  寄存器：

  RDH = Receive Descriptor Head  硬件生产位置
  RDT = Receive Descriptor Tail  软件归还位置

  RX 和 TX 最容易混淆。RX 里软件不是“填待发送包”，而是补充空 buffer 给硬件。

  初始化时：

  for (int i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    rx_ring[i].addr = (uint64)rx_mbufs[i]->head;
  }
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;

  见 kernel/e1000.c:70。

  为什么 RDT = RX_RING_SIZE - 1？

  因为 E1000 的接收规则是：

  硬件可以使用 RDH 到 RDT 之间的描述符
  RDT 表示软件已经准备好的最后一个描述符

  初始化时设成最后一个，表示整个 ring 都可供硬件使用。

  收到包后：

  硬件：
    1. 找一个可用 RX 描述符
    2. DMA 把包写到 desc.addr 指向的 mbuf
    3. 写 desc.length
    4. 设置 desc.status.DD = 1
    5. 如果一个完整包结束，设置 EOP
    6. 推进 RDH
    7. 触发 RXDW 中断

  软件中断里处理：

  int index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  struct rx_desc *desc = &rx_ring[index];

  if ((desc->status & E1000_RXD_STAT_DD) == 0)
    return;

  见 kernel/e1000.c:152。

  为什么软件看 RDT + 1？

  因为 RDT 是“软件最后归还给硬件的位置”。如果硬件已经填好了下一个描述符，那么软件就从 RDT + 1 开始取。

  软件消费一个包后：

  1. 取出 rx_mbufs[index] 作为收到的包
  2. 分配 replacement mbuf
  3. 把 replacement->head 写回 desc.addr
  4. 清 desc.status
  5. RDT = index
     这表示这个描述符重新归还给硬件
  6. 把原 mbuf 交给 net_rx()

  核心代码：

  struct mbuf *m = rx_mbufs[index];
  replacement = mbufalloc(0);

  m->len = desc->length;
  rx_mbufs[index] = replacement;
  desc->addr = (uint64)replacement->head;

  desc->status = 0;
  __sync_synchronize();
  regs[E1000_RDT] = index;

  见 kernel/e1000.c:161。

  可以画成：

  RX ring：硬件生产，软件消费

         软件准备好的最后一个
                |
                v
  +----+----+----+----+----+
  | D0 | D1 | D2 | D3 | D4 |
  +----+----+----+----+----+
         ^         ^
         |         |
        RDH       RDT

  RDH -> 硬件下一次可能写入的位置
  RDT -> 软件最后归还给硬件的位置

  软件实际消费的是：

  (RDT + 1) % RX_RING_SIZE

  如果这个位置 DD=1，说明硬件已经写好了包。

  ———

  TX/RX 对比

  TX:
    软件填描述符
    软件推进 TDT
    硬件读取描述符和包
    硬件设置 DD
    硬件推进 TDH

  RX:
    软件预先放空 buffer
    软件推进 RDT 表示归还 buffer
    硬件写入包
    硬件设置 DD
    硬件推进 RDH
    软件从 RDT+1 取包

  更紧凑地说：

  TX 的 tail 是“软件通知硬件：我又放了一个待发送包”
  RX 的 tail 是“软件通知硬件：我又还了一个空接收 buffer”

  为什么两边都要 DD 位

  因为 head/tail 是设备寄存器，软件读它们不一定是最方便或最可靠的同步方式。描述符里的 DD 是硬件对单个描述符的完成确认。

  在 xv6 里：

  TX:
    软件检查 DD=1 才能重用该 TX 描述符

  RX:
    软件检查 DD=1 才知道该 RX 描述符里有新包

  这个驱动里的所有权变化

  TX：

  软件/协议栈 owns mbuf
    -> e1000_transmit 成功
    -> 驱动/硬件 owns mbuf
    -> 硬件设置 DD
    -> e1000_reclaim 释放 mbuf

  RX：

  驱动 owns 空 mbuf
    -> 硬件 DMA 写入
    -> DD=1
    -> 驱动取出 mbuf
    -> 补 replacement 给硬件
    -> 原 mbuf 交给 net_rx()

  所以 head/tail 的配合，本质上是在维护这件事：

  哪些描述符属于软件
  哪些描述符属于硬件
  哪些 mbuf 可以释放
  哪些 mbuf 必须保留给 DMA 使用

