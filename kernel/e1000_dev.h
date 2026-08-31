#ifndef XV6_E1000_DEV_H
#define XV6_E1000_DEV_H

// Intel 82540EM（E1000）寄存器定义。驱动把 MMIO 基址作为 uint32 数组
// 访问，因此手册中的字节偏移需要除以 4。
#define E1000_CTL   (0x00000 / 4)
#define E1000_ICR   (0x000C0 / 4)
#define E1000_IMS   (0x000D0 / 4)
#define E1000_IMC   (0x000D8 / 4)
#define E1000_RCTL  (0x00100 / 4)
#define E1000_TCTL  (0x00400 / 4)
#define E1000_TIPG  (0x00410 / 4)
#define E1000_RDBAL (0x02800 / 4)
#define E1000_RDBAH (0x02804 / 4)
#define E1000_RDLEN (0x02808 / 4)
#define E1000_RDH   (0x02810 / 4)
#define E1000_RDT   (0x02818 / 4)
#define E1000_RDTR  (0x02820 / 4)
#define E1000_RADV  (0x0282C / 4)
#define E1000_TDBAL (0x03800 / 4)
#define E1000_TDBAH (0x03804 / 4)
#define E1000_TDLEN (0x03808 / 4)
#define E1000_TDH   (0x03810 / 4)
#define E1000_TDT   (0x03818 / 4)
#define E1000_MTA   (0x05200 / 4)
#define E1000_RA    (0x05400 / 4)

// 设备控制、发送控制和接收控制寄存器中的常用位。
#define E1000_CTL_SLU 0x00000040
#define E1000_CTL_RST 0x04000000

#define E1000_TCTL_EN         0x00000002
#define E1000_TCTL_PSP        0x00000008
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_RCTL_EN      0x00000002
#define E1000_RCTL_BAM     0x00008000
#define E1000_RCTL_SZ_2048 0x00000000
#define E1000_RCTL_SECRC   0x04000000

#define E1000_INT_TXDW (1 << 0)
#define E1000_INT_RXDW (1 << 7)

// 发送描述符：EOP 表示包结束，RS 要求硬件回写 DD 完成位。
#define E1000_TXD_CMD_EOP 0x01
#define E1000_TXD_CMD_RS  0x08
#define E1000_TXD_STAT_DD 0x01

struct tx_desc {
  uint64 addr;   // DMA 读取的报文物理地址
  uint16 length; // 报文长度
  uint8 cso;
  uint8 cmd;
  uint8 status;
  uint8 css;
  uint16 special;
};

// 接收描述符的 DD/EOP 位分别表示 DMA 完成和单包结束。
#define E1000_RXD_STAT_DD  0x01
#define E1000_RXD_STAT_EOP 0x02

struct rx_desc {
  uint64 addr;   // DMA 写入的接收缓冲区地址
  uint16 length; // 硬件实际写入的报文长度
  uint16 csum;
  uint8 status;
  uint8 errors;
  uint16 special;
};

#endif
