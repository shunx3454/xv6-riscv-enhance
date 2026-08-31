#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

// E1000 使用固定大小的 DMA 描述符环。tx_mbufs/rx_mbufs 记录每个描述符
// 当前关联的 mbuf，从而明确“硬件、驱动、协议栈”之间的所有权。
#define TX_RING_SIZE 16
#define RX_RING_SIZE 16

static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];
static volatile uint32 *regs;
static struct spinlock e1000_lock;

void
e1000_init(uint32 *xregs)
{
  regs = xregs;
  initlock(&e1000_lock, "e1000");

  // 复位期间先屏蔽全部中断，防止描述符环尚未就绪时进入中断处理。
  regs[E1000_IMC] = 0xffffffff;
  regs[E1000_CTL] |= E1000_CTL_RST;
  __sync_synchronize();
  regs[E1000_IMC] = 0xffffffff;

  // 所有发送描述符初始标记为 DD，表示可由软件立即使用。
  memset(tx_ring, 0, sizeof(tx_ring));
  for (int i = 0; i < TX_RING_SIZE; i++)
    tx_ring[i].status = E1000_TXD_STAT_DD;
  regs[E1000_TDBAL] = (uint64)tx_ring;
  regs[E1000_TDBAH] = 0;
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = 0;
  regs[E1000_TDT] = 0;

  // 接收环必须预先为每个描述符提供一个可供网卡 DMA 写入的 mbuf。
  memset(rx_ring, 0, sizeof(rx_ring));
  for (int i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (rx_mbufs[i] == 0)
      panic("e1000 rx mbuf");
    rx_ring[i].addr = (uint64)rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64)rx_ring;
  regs[E1000_RDBAH] = 0;
  regs[E1000_RDLEN] = sizeof(rx_ring);
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;

  // 配置 QEMU 默认 E1000 MAC 地址 52:54:00:12:34:56，并清空组播表。
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x80005634;
  for (int i = 0; i < 4096 / 32; i++)
    regs[E1000_MTA + i] = 0;

  // 启用发送器和接收器。SECRC 让硬件剥离以太网 CRC，BAM 接收广播包。
  regs[E1000_TCTL] = E1000_TCTL_EN | E1000_TCTL_PSP |
                      (0x10 << E1000_TCTL_CT_SHIFT) |
                      (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8 << 10) | (6 << 20);
  regs[E1000_RCTL] = E1000_RCTL_EN | E1000_RCTL_BAM |
                     E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC;
  // 不做接收中断延迟，并开启发送完成与接收完成中断。
  regs[E1000_RDTR] = 0;
  regs[E1000_RADV] = 0;
  (void)regs[E1000_ICR];
  regs[E1000_IMS] = E1000_INT_TXDW | E1000_INT_RXDW;
}

int
e1000_transmit(struct mbuf *m)
{
  acquire(&e1000_lock);
  int index = regs[E1000_TDT];
  struct tx_desc *desc = &tx_ring[index];
  // DD 为 0 表示描述符仍归硬件所有。环满时调用者继续持有 m。
  if ((desc->status & E1000_TXD_STAT_DD) == 0) {
    release(&e1000_lock);
    return -1;
  }
  // 描述符上一次发送已完成，可以释放旧 mbuf。
  if (tx_mbufs[index])
    mbuffree(tx_mbufs[index]);

  desc->addr = (uint64)m->head;
  desc->length = m->len;
  desc->cso = 0;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  desc->status = 0;
  desc->css = 0;
  desc->special = 0;
  // 从这里开始 m 的所有权交给驱动，直到硬件回写 DD 后再释放。
  tx_mbufs[index] = m;
  // 保证描述符内容先于 TDT 更新对设备可见。
  __sync_synchronize();
  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_reclaim(void)
{
  // TXDW 中断到达后回收所有已由硬件置 DD 的发送缓冲区。
  acquire(&e1000_lock);
  for (int i = 0; i < TX_RING_SIZE; i++) {
    if (tx_mbufs[i] && (tx_ring[i].status & E1000_TXD_STAT_DD)) {
      mbuffree(tx_mbufs[i]);
      tx_mbufs[i] = 0;
    }
  }
  release(&e1000_lock);
}

static void
e1000_recv(void)
{
  for (;;) {
    acquire(&e1000_lock);
    int index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    struct rx_desc *desc = &rx_ring[index];
    // 从 RDT 的下一个描述符开始消费；DD 为 0 时当前没有新包。
    if ((desc->status & E1000_RXD_STAT_DD) == 0) {
      release(&e1000_lock);
      return;
    }
    __sync_synchronize();

    struct mbuf *m = rx_mbufs[index];
    struct mbuf *replacement = 0;
    int deliver = 0;
    // 本驱动要求一个包完整放在一个描述符中。只有 EOP、无错误且长度
    // 合法时才上送；上送前必须先补充新的 DMA 缓冲区。
    if ((desc->status & E1000_RXD_STAT_EOP) && desc->errors == 0 &&
        desc->length <= MBUF_SIZE) {
      replacement = mbufalloc(0);
      if (replacement) {
        m->len = desc->length;
        rx_mbufs[index] = replacement;
        desc->addr = (uint64)replacement->head;
        deliver = 1;
      }
    }
    // 畸形包或内存不足时丢包，并复用原 mbuf，避免接收环出现空洞。
    if (!deliver) {
      m->next = 0;
      m->head = m->buf;
      m->len = 0;
      desc->addr = (uint64)m->head;
    }

    desc->length = 0;
    desc->csum = 0;
    desc->status = 0;
    desc->errors = 0;
    desc->special = 0;
    __sync_synchronize();
    // 清空状态后推进 RDT，把描述符重新交还给硬件。
    regs[E1000_RDT] = index;
    release(&e1000_lock);

    // net_rx() 可能立即发送 ARP/ICMP 应答，因此绝不能持有 e1000_lock。
    if (deliver)
      net_rx(m);
  }
}

void
e1000_intr(void)
{
  // ICR 是读清寄存器；根据中断原因分别回收 TX 和处理 RX。
  uint32 cause = regs[E1000_ICR];
  if (cause & E1000_INT_TXDW)
    e1000_reclaim();
  if (cause & E1000_INT_RXDW)
    e1000_recv();
}
