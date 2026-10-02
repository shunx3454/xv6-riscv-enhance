#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// entry.S jumps here in machine mode on stack0.
void
start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  // 设置 当mret 返回时，返回到 s mode 模式
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  // 设置 mret 返回 的 pc 为 main函数
  w_mepc((uint64)main);

  // disable paging for now.
  // 关闭页表
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // 异常处理由 s mode 代理
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  // 关闭页表时 MPU 内存保护设置
  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // enable hardware updates of page table A and D bits
  // 页表的 A D位设置
  w_menvcfg(r_menvcfg() | MENVCFG_ADUE);

  // ask for clock interrupts.
  // 时钟中断开启
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  // hart id 是再m mode 读取，s mode 需要自己随时保存
  int id = r_mhartid();
  w_tp(id);

  // switch to supervisor mode and jump to main().
  // 返回 进入 s mode，回到main函数
  asm volatile("mret");
}

// ask each hart to generate timer interrupts.
void
timerinit()
{
  // enable the sstc extension (i.e. stimecmp).
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);

  // allow supervisor to use stimecmp and time.
  w_mcounteren(r_mcounteren() | 2);

  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
