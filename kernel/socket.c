#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "net.h"
#include "socket.h"

// 所有 UDP socket 共用一把表锁。锁只保护 socket 元数据与接收队列；
// copyin/copyout、协议栈发送和 mbuf 释放都尽量放到锁外执行。
static struct {
  struct spinlock lock;
  struct socket sockets[NSOCKET];
  uint32 next_ephemeral;
} stable;

void
socketinit(void)
{
  initlock(&stable.lock, "socket");
  stable.next_ephemeral = EPHEMERAL_FIRST;
}

struct socket *
socketalloc(void)
{
  // socket 使用固定表分配，不额外申请内核页。
  acquire(&stable.lock);
  for (int i = 0; i < NSOCKET; i++) {
    struct socket *s = &stable.sockets[i];
    if (!s->used) {
      memset(s, 0, sizeof(*s));
      s->used = 1;
      release(&stable.lock);
      return s;
    }
  }
  release(&stable.lock);
  return 0;
}

void
socketclose(struct socket *s)
{
  // 先在锁内使表项失效并唤醒阻塞接收者，再在锁外释放队列中的 mbuf。
  acquire(&stable.lock);
  struct mbuf *m = s->rx_head;
  memset(s, 0, sizeof(*s));
  wakeup(s);
  release(&stable.lock);
  while (m) {
    struct mbuf *next = m->next;
    mbuffree(m);
    m = next;
  }
}

static int
port_available(uint16 port, struct socket *self)
{
  // 第一版不支持 SO_REUSEPORT，一个本地端口只能属于一个 socket。
  for (int i = 0; i < NSOCKET; i++) {
    struct socket *s = &stable.sockets[i];
    // 别人sock && 已使用 && 已绑定port && 端口已绑定
    if (s != self && s->used && s->bound && s->local_port == port)
      return 0;
  }
  return 1;
}

// 调用者必须持有 stable.lock；从 IANA 动态端口范围循环查找空闲端口。
static int
socket_autobind(struct socket *s)
{
  if (s->bound)
    return 0;
  uint32 count = EPHEMERAL_LAST - EPHEMERAL_FIRST + 1;
  for (uint32 n = 0; n < count; n++) {
    uint16 port = stable.next_ephemeral;
    stable.next_ephemeral++;
    if (stable.next_ephemeral > EPHEMERAL_LAST)
      stable.next_ephemeral = EPHEMERAL_FIRST;
    if (port_available(port, s)) {
      s->bound = 1;
      s->local_ip = INADDR_ANY;
      s->local_port = port;
      return 0;
    }
  }
  return -1;
}

int
socket_bind(struct socket *s, uint32 ip, uint16 port)
{
  // 只允许绑定 INADDR_ANY 或本机固定地址；端口 0 表示自动绑定。
  if (ip != INADDR_ANY && ip != net_local_ip())
    return -1;
  acquire(&stable.lock);
  if (!s->used || s->bound) {
    release(&stable.lock);
    return -1;
  }
  if (port == 0) {
    int ret = socket_autobind(s);
    // 端口分配成功，ip绑定成功
    if (ret == 0)
      s->local_ip = ip;
    release(&stable.lock);
    return ret;
  }
  if (!port_available(port, s)) {
    release(&stable.lock);
    return -1;
  }
  s->bound = 1;
  s->local_ip = ip;
  s->local_port = port;
  release(&stable.lock);
  return 0;
}

int
socket_connect(struct socket *s, uint32 ip, uint16 port)
{
  // UDP connect 不建立连接，只设置默认目的地址并启用接收端对端过滤。
  if (ip == 0 || port == 0)
    return -1;
  acquire(&stable.lock);
  if (!s->used || socket_autobind(s) < 0) {
    release(&stable.lock);
    return -1;
  }
  s->connected = 1;
  s->remote_ip = ip;
  s->remote_port = port;
  release(&stable.lock);
  return 0;
}

int
socket_sendto(struct socket *s, uint64 addr, int len, int has_dst,
              uint32 dst_ip, uint16 dst_port)
{
  if (len < 0 || len > UDP_MAX_PAYLOAD)
    return -1;

  // 在锁内完成自动绑定并快照发送所需信息，随后不再持锁进入协议栈。
  acquire(&stable.lock);
  if (!s->used || socket_autobind(s) < 0) {
    release(&stable.lock);
    return -1;
  }
  if (!has_dst) {
    if (!s->connected) {
      // 没有rmeote地址，之前还没绑定，出错
      release(&stable.lock);
      return -1;
    }
    dst_ip = s->remote_ip;
    dst_port = s->remote_port;
  }
  uint16 sport = s->local_port;
  release(&stable.lock);
  if (dst_ip == 0 || dst_port == 0)
    return -1;

  // 用户数据只复制一次，之后同一个 mbuf 依次压入 UDP/IP/Ethernet 首部。
  // head = m + NET_HEADROOM
  struct mbuf *m = mbufalloc(NET_HEADROOM);
  if (m == 0)
    return -1;
    // 末尾追加UDP数据负载，m的len会变大，data是追加位置
  char *data = mbufput(m, len);
  struct proc *p = myproc();
  if (len && copyin(p->pagetable, p->sz, data, addr, len) < 0) {
    mbuffree(m);
    return -1;
  }
  if (udp_tx(m, sport, dst_ip, dst_port) < 0) {
    mbuffree(m);
    return -1;
  }
  return len;
}

int
socket_recvfrom(struct socket *s, uint64 addr, int len, uint64 srcaddr,
                uint64 addrlenp)
{
  struct proc *p = myproc();
  int capacity = 0;
  if (srcaddr) {
    if (addrlenp == 0 ||
        copyin(p->pagetable, p->sz, (char *)&capacity, addrlenp,
               sizeof(capacity)) < 0 ||
        capacity < sizeof(struct sockaddr_in))
      return -1;
  }

  // 接收队列为空时按 socket 地址睡眠。sleep_prepare() 消除解锁与睡眠
  // 之间的丢失唤醒窗口，while 循环负责重新检查条件。
  acquire(&stable.lock);
  while (s->used && s->rx_head == 0) {
    if (killed(p)) {
      release(&stable.lock);
      return -1;
    }
    sleep_prepare(s);
    release(&stable.lock);
    sleep();
    acquire(&stable.lock);
  }
  if (!s->used) {
    release(&stable.lock);
    return -1;
  }
  // 一次调用只出队一个 mbuf，因此始终保持 UDP 数据报边界。
  struct mbuf *m = s->rx_head;
  s->rx_head = m->next;
  if (s->rx_head == 0)
    s->rx_tail = 0;
  s->rx_count--;
  m->next = 0;
  release(&stable.lock);

  // 出队后立即释放全局锁，再向用户空间复制 payload 和来源地址。
  int n = len;
  if ((uint)n > m->len)
    n = m->len;
  int ret = n;
  if (n && copyout(p->pagetable, p->sz, addr, m->head, n) < 0)
    ret = -1;
  if (ret >= 0 && srcaddr) {
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(m->src_port);
    sin.sin_addr = htonl(m->src_ip);
    int actual = sizeof(sin);
    // buf      ← UDP payload
    // peer     ← 发送方 IP 和端口
    // peerlen  ← 实际 sockaddr 长度
    if (copyout(p->pagetable, p->sz, srcaddr, (char *)&sin, sizeof(sin)) < 0 ||
        copyout(p->pagetable, p->sz, addrlenp, (char *)&actual,
                sizeof(actual)) < 0)
      ret = -1;
  }
  mbuffree(m);
  return ret;
}

int
socketread(struct socket *s, uint64 addr, int len)
{
  // read(socket) 等价于忽略来源地址的 recvfrom()。
  if (len < 0)
    return -1;
  return socket_recvfrom(s, addr, len, 0, 0);
}

int
socketwrite(struct socket *s, uint64 addr, int len)
{
  // write(socket) 只能用于已 connect 的 UDP socket。
  return socket_sendto(s, addr, len, 0, 0, 0);
}

// 被 udp 层级调用
// UDP 接收分用：先匹配本地端口/地址，connected socket 还要匹配对端。
void
socket_rx_udp(struct mbuf *m, uint32 src_ip, uint16 src_port, uint32 dst_ip,
              uint16 dst_port)
{
  struct socket *match = 0;
  acquire(&stable.lock);

  // s->used
  // socket 表项必须已经分配。
  // s->bound
  // socket 必须拥有本地端口。客户端即使没有显式调用 bind()，第一次 connect() 或 sendto() 也会自动绑定临时端口。
  // s->local_port == dst_port
  // 收到的 UDP 目的端口必须等于 socket 的本地端口。
  for (int i = 0; i < NSOCKET; i++) {
    struct socket *s = &stable.sockets[i];
    if (!s->used || !s->bound || s->local_port != dst_port)
      continue;

    // 绑定到任意本地地址：
    // s->local_ip == INADDR_ANY // 0.0.0.0
    // 10.0.2.15:2000
    // 绑定到具体本地地址：
    if (s->local_ip != INADDR_ANY && s->local_ip != dst_ip)
      continue;

      // 如果显示特殊连接 进行过滤
    if (s->connected &&
        (s->remote_ip != src_ip || s->remote_port != src_port))
      continue;
    match = s;
    break;
  }
  // 队列未满时转移 mbuf 所有权并唤醒 recvfrom()；否则直接丢包。
  if (match && match->rx_count < UDP_RXQ_MAX) {
    m->next = 0;
    if (match->rx_tail)
      match->rx_tail->next = m;
    else
      match->rx_head = m;
    match->rx_tail = m;
    match->rx_count++;
    wakeup(match);
    release(&stable.lock);
    return;
  }
  release(&stable.lock);
  mbuffree(m);
}
