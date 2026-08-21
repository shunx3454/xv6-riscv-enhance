# xv6-riscv UART 与外部中断处理指南

本文精炼总结 xv6-riscv 中 UART 中断、PLIC 分发、`devintr()`、`uartintr()`、console 输入输出，以及多 hart 下外部中断如何被 claim/complete。

相关源码：

- [`kernel/uart.c`](../kernel/uart.c)：UART 驱动、输入输出、中断处理。
- [`kernel/console.c`](../kernel/console.c)：console 行编辑、输入缓冲、字符回显。
- [`kernel/trap.c`](../kernel/trap.c)：`devintr()` 分发外部中断和 timer interrupt。
- [`kernel/plic.c`](../kernel/plic.c)：PLIC 初始化、claim、complete。
- [`kernel/memlayout.h`](../kernel/memlayout.h)：UART、virtio、PLIC MMIO 地址和 IRQ 号。
- [`kernel/riscv.h`](../kernel/riscv.h)：`scause`、`sie`、`sstatus` 等 CSR 封装。

## 1. UART 和 PLIC 的位置

QEMU `virt` 平台上的相关地址在 [`kernel/memlayout.h`](../kernel/memlayout.h)：

```c
#define UART0     0x10000000L
#define UART0_IRQ 10

#define VIRTIO0     0x10001000
#define VIRTIO0_IRQ 1

#define PLIC                 0x0c000000L
#define PLIC_SENABLE(hart)   (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)    (PLIC + 0x201004 + (hart) * 0x2000)
```

关系：

```text
UART 设备产生 IRQ 10
  -> PLIC 接收并分发 external interrupt
  -> 某个 hart trap 到 S-mode
  -> trap.c:devintr()
  -> uart.c:uartintr()
```

## 2. PLIC 初始化

全局初始化在 [`kernel/plic.c`](../kernel/plic.c)：

```c
void
plicinit(void)
{
  *(uint32 *)(PLIC + UART0_IRQ * 4) = 1;
  *(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;
}
```

含义：

```text
UART IRQ 优先级 = 1
virtio IRQ 优先级 = 1
优先级非 0 才会被 PLIC 分发
```

每个 hart 初始化：

```c
void
plicinithart(void)
{
  int hart = cpuid();

  *(uint32 *)PLIC_SENABLE(hart) =
    (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  *(uint32 *)PLIC_SPRIORITY(hart) = 0;
}
```

含义：

```text
每个 hart 的 S-mode 都允许接收 UART 和 virtio 外部中断
priority threshold = 0，优先级大于 0 的 IRQ 可以投递
```

## 3. 外部中断进入 `devintr()`

外部中断到来时，RISC-V `scause` 为：

```text
0x8000000000000009  supervisor external interrupt
```

xv6 在 [`kernel/trap.c`](../kernel/trap.c) 的 `devintr()` 中处理：

```c
if (scause == 0x8000000000000009L) {
  int irq = plic_claim();

  if (irq == UART0_IRQ) {
    uartintr();
  } else if (irq == VIRTIO0_IRQ) {
    virtio_disk_intr();
  }

  if (irq)
    plic_complete(irq);

  return 1;
}
```

流程：

```text
CPU 只知道来了 external interrupt
PLIC claim 告诉 CPU 具体是哪一个 IRQ
irq == UART0_IRQ   -> uartintr()
irq == VIRTIO0_IRQ -> virtio_disk_intr()
处理后 complete
```

## 4. PLIC 多 hart 分发与 claim 语义

xv6 让每个 hart 都 enable UART/virtio，所以多个 hart context 都可能满足接收条件。

但具体某个 pending IRQ 只会被一个 hart claim 到。这是 PLIC 的硬件/平台语义保证：

```text
读 claim:
  返回当前 hart context 可处理的最高优先级 pending IRQ
  同时把该 pending IRQ 标记为已领取

写 complete:
  告诉 PLIC 该 IRQ 已处理完成
```

xv6 的 `plic_claim()` 没有加锁：

```c
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32 *)PLIC_SCLAIM(hart);
  return irq;
}
```

原因是：

```text
claim 寄存器读取本身就是原子领取语义
同一次 pending IRQ 不会被多个 hart 重复 claim
```

但要注意：

```text
同一设备的后续中断可以被不同 hart 处理
PLIC 不保证设备驱动内部共享数据安全
```

所以 console、UART、virtio 等驱动内部仍需要锁。

## 5. UART 寄存器和初始化

UART 驱动在 [`kernel/uart.c`](../kernel/uart.c)。

寄存器通过 MMIO 访问：

```c
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))
#define ReadReg(reg)     (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))
```

关键寄存器：

```text
RHR  receive holding register，读输入字符
THR  transmit holding register，写输出字符
IER  interrupt enable register
ISR  interrupt status register
LCR  line control register
LSR  line status register
```

关键状态位：

```text
LSR_RX_READY  输入数据可读
LSR_TX_IDLE   发送寄存器空，可继续写
```

`uartinit()` 会：

```text
关闭 UART 中断
设置波特率
设置 8-bit、no parity
清空并启用 FIFO
启用 RX/TX interrupt
初始化 tx_lock
```

其中：

```c
WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
```

表示 UART 可以因为输入到达或发送可继续而产生中断。

## 6. `uartintr()` 做什么

核心代码：

```c
void
uartintr(void)
{
  ReadReg(ISR); // acknowledge the interrupt

  if (ReadReg(LSR) & LSR_TX_IDLE) {
    // UART finished transmitting; wake up sending thread.
    wakeup(&tx_chan);
  }

  // read and process incoming characters, if any.
  while (1) {
    int c = uartgetc();
    if (c == -1)
      break;
    consoleintr(c);
  }
}
```

逐句解释：

```text
ReadReg(ISR)
  读取 UART 中断状态，确认设备层面的中断

if LSR_TX_IDLE
  UART 发送寄存器空了
  唤醒等待发送的线程

while uartgetc()
  尽量一次性读空 UART 接收 FIFO

consoleintr(c)
  把输入字符交给 console 层处理
```

UART 中断可能同时表示：

```text
输入字符到达
UART 可以继续输出
```

所以 `uartintr()` 同时处理 RX 和 TX。

## 7. TX：输出侧中断流程

普通用户 `write()` 到 console 最终走：

```text
consolewrite()
  -> uartwrite()
```

`uartwrite()` 中：

```c
while (i < n) {
  sleep_prepare(&tx_chan);
  if (ReadReg(LSR) & LSR_TX_IDLE) {
    WriteReg(THR, buf[i]);
    i += 1;
  } else {
    sleep();
  }
}
```

含义：

```text
UART 空闲:
  写一个字节到 THR

UART 忙:
  当前发送线程睡在 tx_chan 上
```

当 UART 发送完一个字节后，会产生 TX interrupt：

```text
UART TX empty
  -> PLIC
  -> devintr()
  -> uartintr()
  -> LSR_TX_IDLE
  -> wakeup(&tx_chan)
  -> uartwrite() 继续写下一个字节
```

TX 输出路径：

```text
write()
  -> consolewrite()
  -> uartwrite()
  -> UART 忙则 sleep
  -> UART TX interrupt
  -> uartintr()
  -> wakeup(&tx_chan)
  -> 继续发送
```

## 8. RX：输入侧中断流程

输入字符到达时：

```text
用户键盘输入
  -> UART RX FIFO
  -> UART RX interrupt
  -> PLIC
  -> devintr()
  -> uartintr()
```

`uartgetc()`：

```c
static int
uartgetc(void)
{
  if (ReadReg(LSR) & LSR_RX_READY) {
    return ReadReg(RHR);
  } else {
    return -1;
  }
}
```

`uartintr()` 用循环读空接收 FIFO：

```text
有字符:
  读 RHR
  consoleintr(c)

没有字符:
  break
```

## 9. console 层如何处理输入

`uartintr()` 把每个输入字符交给 [`kernel/console.c`](../kernel/console.c) 的 `consoleintr(c)`。

`consoleintr()` 负责：

```text
Ctrl-P  打印进程列表
Ctrl-U  删除当前行
Backspace/Delete 退格
'\r' 转成 '\n'
回显字符
放入 cons.buf 输入缓冲
收到完整一行后 wakeup(&cons.r)
```

关键逻辑：

```c
c = (c == '\r') ? '\n' : c;
consputc(c);
cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

if (c == '\n' || c == C('D') || cons.e - cons.r == INPUT_BUF_SIZE) {
  cons.w = cons.e;
  wakeup(&cons.r);
}
```

RX 输入路径：

```text
UART RX interrupt
  -> uartintr()
  -> uartgetc()
  -> consoleintr(c)
  -> cons.buf
  -> 完整一行后 wakeup(&cons.r)
  -> consoleread()
  -> read(0, ...)
```

## 10. 同步与锁

UART/console 中断路径涉及多个并发来源：

```text
用户进程 write()
用户进程 read()
UART 中断
多个 hart 可能处理不同次设备中断
```

相关同步：

```text
tx_lock      sleeplock，序列化 uartwrite()
tx_chan      UART TX 等待 channel
cons.lock    spinlock，保护 console 输入缓冲
PLIC claim   硬件保证同一次 pending IRQ 只被一个 hart claim
```

注意：

```text
PLIC 只保证 IRQ claim/complete 不重复
不保护驱动内部数据结构
```

所以 `consoleintr()` 仍然要：

```c
acquire(&cons.lock);
...
release(&cons.lock);
```

## 11. `uartputc_sync()` 与中断驱动输出

`uartputc_sync()`：

```c
while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
  ;
WriteReg(THR, c);
```

特点：

```text
忙等 UART 空闲
不依赖 TX interrupt
用于 printk、panic、输入回显等路径
```

`uartwrite()`：

```text
UART 忙时 sleep
等待 TX interrupt wakeup
适合普通 write() 系统调用
```

对比：

| 路径 | 等待方式 | 典型用途 |
|---|---|---|
| `uartputc_sync()` | 忙等 | `printk()`、panic、echo |
| `uartwrite()` + `uartintr()` | sleep/wakeup | 用户 `write()` |

## 12. 图形串口换行错位问题

如果不用 `-nographic`，改用 QEMU 图形界面的 `serial0`，可能看到输出呈阶梯状：

```text
xv6 kernel is booting
                    hart 2 starting
                                   hart 1 starting
```

原因：

```text
xv6 输出主要发 '\n'
某些图形 serial 前端不会自动把 LF 转成 CRLF
'\n' 只下移一行，不回到行首
```

终端模式 `-nographic` 通常由宿主终端处理换行，所以不明显。

如果要在图形 serial0 中显示正常，可以在 console/UART 输出层把：

```text
\n
```

转换成：

```text
\r\n
```

但需要统一选择转换位置，避免重复输出 `\r`。

## 13. 总结

完整 UART 外部中断链路：

```text
UART device
  -> PLIC pending IRQ 10
  -> supervisor external interrupt
  -> trap.c:devintr()
  -> plic_claim()
  -> uartintr()
      TX_IDLE -> wakeup(&tx_chan)
      RX_READY -> uartgetc() -> consoleintr(c)
  -> plic_complete()
```

关键理解：

```text
PLIC 负责把外部设备 IRQ 分发给 hart
claim/complete 是硬件级领取/完成语义
同一次 pending IRQ 只会被一个 hart claim
同一设备的后续中断可以由不同 hart 处理
UART 中断同时覆盖输入到达和输出可继续
console 层负责行编辑、回显、输入缓冲和唤醒 read()
驱动内部共享状态仍要靠锁保护
```
