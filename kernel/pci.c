#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// PCIe 设备由 BDF（Bus/Device/Function）唯一标识。QEMU virt 提供
// Generic ECAM Host Bridge，把每个 Function 的 4KB PCI 配置空间映射到
// CPU 物理地址空间：
//
//   ECAM_BASE + (bus << 20) + (device << 15) + (function << 12)
//
// 当前 xv6 只扫描 bus 0，但会扫描其中 32 个 device 及多功能设备的
// 0~7 个 function。ECAM 仅用于发现和配置设备；E1000 真正的运行寄存器
// 通过 BAR0 映射到另一段 MMIO 地址。
#define PCI_ECAM_BUS_SHIFT      20
#define PCI_ECAM_DEVICE_SHIFT   15
#define PCI_ECAM_FUNCTION_SHIFT 12

// PCI 配置空间公共首部中的标准偏移。
#define PCI_ID_REG          0x00 // Vendor ID（低 16 位）和 Device ID（高 16 位）
#define PCI_COMMAND_REG     0x04
#define PCI_HEADER_TYPE_REG 0x0e
#define PCI_BAR0_REG        0x10

// Command Register：允许 CPU 访问 BAR、允许设备作为总线主设备发起 DMA。
#define PCI_COMMAND_IO     (1 << 0)
#define PCI_COMMAND_MEMORY (1 << 1)
#define PCI_COMMAND_MASTER (1 << 2)

// BAR 低位不是地址，而是地址空间类型等属性；内存 BAR 的地址从 bit 4 开始。
#define PCI_BAR_IO                (1 << 0)
#define PCI_BAR_MEMORY_TYPE_MASK  (3 << 1)
#define PCI_BAR_MEMORY_TYPE_64BIT (2 << 1)
#define PCI_BAR_MEMORY_ADDR_MASK  0xfffffff0U

#define PCI_HEADER_MULTIFUNCTION 0x80
#define PCI_VENDOR_NONE          0xffff
#define PCI_VENDOR_INTEL         0x8086
#define PCI_DEVICE_E1000         0x100e

// 根据 BDF 计算一个 PCI Function 的配置空间基址。真实硬件的 Root
// Complex 会把这里的 load/store 转换成 PCIe Configuration TLP；QEMU 则
// 根据该地址调用相应虚拟 PCIe 设备的配置空间读写回调。
static volatile uint8 *
pci_config_base(uint bus, uint device, uint function)
{
  uint64 offset = ((uint64)bus << PCI_ECAM_BUS_SHIFT) |
                  ((uint64)device << PCI_ECAM_DEVICE_SHIFT) |
                  ((uint64)function << PCI_ECAM_FUNCTION_SHIFT);
  return (volatile uint8 *)(PCIE_ECAM + offset);
}

static uint16
pci_read16(volatile uint8 *config, uint offset)
{
  return *(volatile uint16 *)(config + offset);
}

static uint32
pci_read32(volatile uint8 *config, uint offset)
{
  return *(volatile uint32 *)(config + offset);
}

static void
pci_write16(volatile uint8 *config, uint offset, uint16 value)
{
  *(volatile uint16 *)(config + offset) = value;
}

static void
pci_write32(volatile uint8 *config, uint offset, uint32 value)
{
  *(volatile uint32 *)(config + offset) = value;
}

// 探测 32 位内存 BAR 的大小。PCI 规定：向 BAR 写全 1 后读回，设备会把
// 自己实现的地址位返回为 0。对地址掩码取反加一即可得到所需空间大小。
// 探测结束必须恢复原值，且探测期间应关闭地址译码。
static uint32
pci_probe_mem_bar32(volatile uint8 *config, uint bar_offset, uint32 original)
{
  pci_write32(config, bar_offset, 0xffffffffU);
  __sync_synchronize();
  uint32 mask = pci_read32(config, bar_offset);
  pci_write32(config, bar_offset, original);
  __sync_synchronize();

  mask &= PCI_BAR_MEMORY_ADDR_MASK;
  if (mask == 0)
    return 0;
  return ~mask + 1;
}

// 为已经找到的 E1000 配置 BAR0 和 Command Register，然后把控制权交给
// E1000 驱动。此处使用预留的固定地址而不是实现通用 PCI 资源分配器。
static void
pci_configure_e1000(volatile uint8 *config)
{
  uint16 command = pci_read16(config, PCI_COMMAND_REG);

  // BAR 探测/重编程时暂时关闭 I/O 和 Memory Space 地址译码，避免设备在
  // BAR 值变化过程中响应到错误地址。Bus Master 稍后与 MMIO 一起启用。
  pci_write16(config, PCI_COMMAND_REG,
              command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
  __sync_synchronize();

  // 确定 BAR地址 不是 IO类型
  uint32 bar0 = pci_read32(config, PCI_BAR0_REG);
  if (bar0 & PCI_BAR_IO)
    panic("e1000 BAR0 is I/O");
    // 确定 BAR 地址是32位
  if ((bar0 & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_TYPE_64BIT)
    panic("e1000 BAR0 is 64-bit");

  uint32 bar0_size = pci_probe_mem_bar32(config, PCI_BAR0_REG, bar0);
  if (bar0_size == 0 || (bar0_size & (bar0_size - 1)) != 0 ||
      bar0_size > E1000_MMIO_SIZE || (E1000_MMIO & (bar0_size - 1)) != 0)
    panic("bad e1000 BAR0 size");

  // BAR0 的低 4 位保存属性，因此只替换地址部分。写入后，CPU 对
  // E1000_MMIO 区间的访问会被 PCIe Host Bridge 路由到 E1000 BAR0。
  pci_write32(config, PCI_BAR0_REG,
              (uint32)E1000_MMIO | (bar0 & ~PCI_BAR_MEMORY_ADDR_MASK));
  __sync_synchronize();
  (void)pci_read32(config, PCI_BAR0_REG); // 读回以冲刷可能的 posted write

  // 驱动只使用 MMIO BAR，不需要开启 I/O Space。MASTER 是 E1000 读取 TX
  // 描述符/mbuf、写入 RX 描述符/mbuf 的必要条件。
  pci_write16(config, PCI_COMMAND_REG,
              command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
  __sync_synchronize();
  (void)pci_read16(config, PCI_COMMAND_REG);

  e1000_init((uint32 *)E1000_MMIO);
}

// 在 QEMU virt 的 PCIe ECAM 中寻找 Intel 82540EM E1000。完整 PCI 子系统
// 还需要递归扫描 bridge、分配多个 BAR、处理 MSI/MSI-X 等；本实现只完成
// xv6 启动固定 E1000 所需的最小枚举和资源配置。
void
pci_init(void)
{
  // 固定扫描 bus 0的设备
  const uint bus = 0;

  for (uint device = 0; device < 32; device++) {
    volatile uint8 *config0 = pci_config_base(bus, device, 0);
    uint32 id0 = pci_read32(config0, PCI_ID_REG);
    if ((id0 & 0xffff) == PCI_VENDOR_NONE)
      continue;

    // Header Type bit 7 表示多功能设备；普通设备只需要检查 function 0。
    uint functions = (config0[PCI_HEADER_TYPE_REG] & PCI_HEADER_MULTIFUNCTION)
                       ? 8
                       : 1;
    for (uint function = 0; function < functions; function++) {
      volatile uint8 *config = pci_config_base(bus, device, function);
      uint32 id = pci_read32(config, PCI_ID_REG);
      uint16 vendor = id & 0xffff;
      uint16 product = id >> 16;

      if (vendor == PCI_VENDOR_NONE)
        continue;
      if (vendor == PCI_VENDOR_INTEL && product == PCI_DEVICE_E1000) {
        pci_configure_e1000(config);
        return;
      }
    }
  }

  panic("e1000 not found");
}
