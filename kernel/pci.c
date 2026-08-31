#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// 在 QEMU virt 机器的 PCIe ECAM 中寻找 82540EM E1000，并把 BAR0
// 映射到内核预留的 E1000_MMIO 地址。该实现只处理 bus 0/function 0。
void
pci_init(void)
{
  uint32 *ecam = (uint32 *)PCIE_ECAM;
  for (int dev = 0; dev < 32; dev++) {
    uint32 off = dev << 11;
    volatile uint32 *base = ecam + off;
    if (base[0] != 0x100e8086)
      continue;

    // 开启 I/O、内存空间和 bus-master DMA，网卡随后才能访问描述符环。
    base[1] = 7;
    __sync_synchronize();
    base[4] = E1000_MMIO;
    __sync_synchronize();
    e1000_init((uint32 *)E1000_MMIO);
    return;
  }
  panic("e1000 not found");
}
