#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "net.h"

// 最小 IPv4 网络层：mbuf -> Ethernet -> ARP/IPv4 -> ICMP/UDP。
// 每个成功下传或上送的函数都会转移 mbuf 所有权；返回失败时调用者释放。
#define NARP 16
#define ARP_PENDING_MAX 8

// INCOMPLETE 表示已发送 ARP 请求但尚未解析出 MAC；此时 IPv4 包暂存在
// pending 队列。REACHABLE 表示可以直接封装 Ethernet 首部发送。
enum arp_state { ARP_EMPTY, ARP_INCOMPLETE, ARP_REACHABLE };

struct arp_entry {
  enum arp_state state;
  uint32 ip;
  uint8 mac[ETHADDR_LEN];
  struct mbuf *pending_head;
  struct mbuf *pending_tail;
  int pending_count;
};

static struct {
  struct spinlock lock; // 同时保护 ARP 表项和待发送队列
  struct arp_entry entry[NARP];
  uint victim; // 被 淘汰的 已缓存的 ARP entry 序号
} arptable;

// QEMU user-net/TAP 测试采用固定网络参数；地址在内核中按主机字节序保存。
static uint8 local_mac[ETHADDR_LEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8 broadcast_mac[ETHADDR_LEN] = {0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff};
static const uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);
static const uint32 netmask = MAKE_IP_ADDR(255, 255, 255, 0);
static const uint32 gateway_ip = MAKE_IP_ADDR(10, 0, 2, 2);
static uint16 ip_id;

void
net_init(void)
{
  initlock(&arptable.lock, "arp");
  memset(arptable.entry, 0, sizeof(arptable.entry));
}

uint32
net_local_ip(void)
{
  return local_ip;
}

// 一个 mbuf 占用一页，headroom 为发送方向预留协议首部空间。
struct mbuf *
mbufalloc(uint headroom)
{
  if (headroom > MBUF_SIZE)
    return 0;
  struct mbuf *m = kalloc();
  if (m == 0)
    return 0;
  m->next = 0;
  m->head = m->buf + headroom;
  m->len = 0;
  m->src_ip = 0;
  m->src_port = 0;
  return m;
}

void
mbuffree(struct mbuf *m)
{
  if (m)
    kfree(m);
}

// 在当前数据前增加 n 字节，供下层协议添加首部。
void *
mbufpush(struct mbuf *m, uint n)
{
  if ((uint)(m->head - m->buf) < n)
    return 0;
  m->head -= n;
  m->len += n;
  return m->head;
}

// 从当前数据前移除 n 字节，供接收路径逐层剥离首部。
void *
mbufpull(struct mbuf *m, uint n)
{
  if (m->len < n)
    return 0;
  char *old = m->head;
  m->head += n;
  m->len -= n;
  return old;
}

// 在当前有效数据末尾追加 n 字节。
void *
mbufput(struct mbuf *m, uint n)
{
  if (n > MBUF_SIZE - (uint)(m->head - m->buf) - m->len)
    return 0;
  char *old = m->head + m->len;
  m->len += n;
  return old;
}

// 从当前有效数据末尾裁掉 n 字节，例如去掉以太网填充。
void *
mbuftrim(struct mbuf *m, uint n)
{
  if (m->len < n)
    return 0;
  m->len -= n;
  return m->head + m->len;
}

// RFC 1071 16 位反码和。逐字节读取可避免未对齐访问。
uint16
inet_checksum(void *data, int len)
{
  uint8 *p = data;
  uint32 sum = 0;
  while (len >= 2) {
    sum += ((uint16)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len)
    sum += (uint16)p[0] << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

// 压入 Ethernet 首部后直接把 mbuf 所有权交给 E1000。
int
eth_tx(struct mbuf *m, const uint8 *dst, uint16 type)
{
  struct eth *eth = mbufpush(m, sizeof(*eth));
  if (eth == 0)
    return -1;
  memmove(eth->dst, dst, ETHADDR_LEN);
  memmove(eth->src, local_mac, ETHADDR_LEN);
  eth->type = htons(type);
  return e1000_transmit(m);
}

// ARP Request 使用广播 MAC，询问目标 IPv4 地址对应的硬件地址。
static void
arp_request(uint32 ip)
{
  struct mbuf *m = mbufalloc(NET_HEADROOM);
  if (m == 0)
    return;
  struct arp *arp = mbufput(m, sizeof(*arp));
  if (arp == 0) {
    mbuffree(m);
    return;
  }
  arp->hrd = htons(ARP_HRD_ETHER); // 硬件类型
  arp->pro = htons(ETHTYPE_IP); // IPv4 协议
  arp->hln = ETHADDR_LEN; // 硬件地址长度
  arp->pln = 4; // 协议地址长度（IPv4 为 4 字节）
  arp->op = htons(ARP_OP_REQUEST); // 操作码（1 为 ARP 请求，2 为 ARP 应答）
  memmove(arp->sha, local_mac, ETHADDR_LEN); // sender mac
  arp->spa = htonl(local_ip); // sender ip
  memset(arp->tha, 0, ETHADDR_LEN); // target mac
  arp->tpa = htonl(ip); // target ip 
  if (eth_tx(m, broadcast_mac, ETHTYPE_ARP) < 0)
    mbuffree(m);
}

// 解析下一跳。成功时 m 的所有权转移给网卡或 INCOMPLETE 表项的待发队列；
// 失败时所有权仍属于调用者。任何跨层发送都在释放 arptable.lock 后进行。
static int
arp_resolve_and_send(uint32 next_hop, struct mbuf *m)
{
  uint8 mac[ETHADDR_LEN];
  int found = -1;
  int slot = -1;

  acquire(&arptable.lock);
  for (int i = 0; i < NARP; i++) {
    // 有ip 且不为 ARP_EMPTY
    if (arptable.entry[i].state != ARP_EMPTY &&
        arptable.entry[i].ip == next_hop) {
      found = i;
      break;
    }
    // ARP 缓冲失效，占用这个arp entry  
    if (slot < 0 && arptable.entry[i].state == ARP_EMPTY)
      slot = i;
  }
  // 命中可达表项：复制 MAC 后先解锁，再进入 Ethernet 层。
  if (found >= 0 && arptable.entry[found].state == ARP_REACHABLE) {
    memmove(mac, arptable.entry[found].mac, ETHADDR_LEN);
    release(&arptable.lock);
    return eth_tx(m, mac, ETHTYPE_IP);
  }

  // 未命中时优先使用空表项；表满后只替换无 pending 包的可达表项。
  if (found < 0) {
    // 一个 Empty arp entry 都没有
    if (slot < 0) {
      for (int n = 0; n < NARP; n++) {
        int i = (arptable.victim + n) % NARP;
        // 找一个 ARP_REACHABLE entry
        if (arptable.entry[i].state == ARP_REACHABLE) {
          slot = i;
          arptable.victim = (i + 1) % NARP;
          break;
        }
      }
    }

    // 全部处于 arp entry INCOMPLETE，arptable 很忙
    if (slot < 0) {
      release(&arptable.lock);
      return -1;
    }

    // 
    struct arp_entry *e = &arptable.entry[slot];
    memset(e, 0, sizeof(*e));
    e->state = ARP_INCOMPLETE;
    e->ip = next_hop;
    found = slot;
  }

  // 每个未解析地址最多积压 ARP_PENDING_MAX 个包，防止内存无限增长。
  // 该 arp entry 下 待发送的 mbuf 太多
  struct arp_entry *e = &arptable.entry[found];
  if (e->pending_count >= ARP_PENDING_MAX) {
    release(&arptable.lock);
    return -1;
  }

  // 正常 把 mbuf 挂到 arp_entry INCOMPLETE 上
  m->next = 0;
  if (e->pending_tail)
    e->pending_tail->next = m;
  else
    e->pending_head = m;
  e->pending_tail = m;
  e->pending_count++;
  release(&arptable.lock);

  // 允许重复发 ARP 请求，使之前分配失败的请求有机会重试。
  arp_request(next_hop);

  // 告诉应用层 已经发送，实际上 arp_entry INCOMPLETE 上 缓存中
  return 0;
}

// 1. 学习并缓存一条 IP → MAC 映射。
// 2. 如果有 IPv4 包正在等待这个映射，立即把它们发送出去
// 从任意合法 ARP 报文学习发送方映射，并在锁内摘下待发送链表。
static void
arp_learn(uint32 ip, const uint8 mac[ETHADDR_LEN])
{
  struct mbuf *pending = 0;
  int slot = -1;

  acquire(&arptable.lock);
  for (int i = 0; i < NARP; i++) {
    if (arptable.entry[i].state != ARP_EMPTY && arptable.entry[i].ip == ip) {
      slot = i;
      break;
    }
    if (slot < 0 && arptable.entry[i].state == ARP_EMPTY)
      slot = i;
  }
  if (slot < 0) {
    for (int n = 0; n < NARP; n++) {
      int i = (arptable.victim + n) % NARP;
      if (arptable.entry[i].state == ARP_REACHABLE) {
        slot = i;
        arptable.victim = (i + 1) % NARP;
        break;
      }
    }
  }
  if (slot >= 0) {
    struct arp_entry *e = &arptable.entry[slot];
    if (e->state == ARP_INCOMPLETE && e->ip == ip) {
      pending = e->pending_head;
    } else {
      e->pending_head = 0;
      e->pending_tail = 0;
      e->pending_count = 0;
    }
    e->state = ARP_REACHABLE;
    e->ip = ip;
    memmove(e->mac, mac, ETHADDR_LEN);
    e->pending_head = 0;
    e->pending_tail = 0;
    e->pending_count = 0;
  }
  release(&arptable.lock);

  // 发送可能进入 E1000，因此必须在 ARP 锁外逐包冲刷 pending 队列。
  while (pending) {
    struct mbuf *next = pending->next;
    pending->next = 0;
    if (eth_tx(pending, mac, ETHTYPE_IP) < 0)
      mbuffree(pending);
    pending = next;
  }
}

// 进入时 Ethernet 首部已经移除；先验证 ARP 固定字段再学习映射。
static void
arp_rx(struct mbuf *m)
{
  if (m->len < sizeof(struct arp)) {
    mbuffree(m);
    return;
  }
  struct arp *arp = (struct arp *)m->head;
  // ARP 只接受 ETH 硬件，ipv4协议
  if (ntohs(arp->hrd) != ARP_HRD_ETHER || ntohs(arp->pro) != ETHTYPE_IP ||
      arp->hln != ETHADDR_LEN || arp->pln != 4) {
    mbuffree(m);
    return;
  }
  uint32 sender_ip = ntohl(arp->spa);
  uint32 target_ip = ntohl(arp->tpa);
  uint16 op = ntohs(arp->op);
  
  // 如果是合法请求，无论是 ARP REQUEST/REPLY 
  // 都可以 学习记录 sender mac <=> sender ip
  // 如果 (sender mac <=> sender ip) 恰好是 arp enrty pending 中需要的，直接记录并 发送 挂起的mbuf
  // 状态：ARP_REACHABLE
  arp_learn(sender_ip, arp->sha);

  // 只为本机 IPv4 地址生成 ARP Reply。
  if (op == ARP_OP_REQUEST && target_ip == local_ip) {
    struct mbuf *reply = mbufalloc(NET_HEADROOM);
    if (reply) {
      struct arp *out = mbufput(reply, sizeof(*out));
      out->hrd = htons(ARP_HRD_ETHER);
      out->pro = htons(ETHTYPE_IP);
      out->hln = ETHADDR_LEN;
      out->pln = 4;
      out->op = htons(ARP_OP_REPLY);
      memmove(out->sha, local_mac, ETHADDR_LEN);
      out->spa = htonl(local_ip);
      memmove(out->tha, arp->sha, ETHADDR_LEN);
      out->tpa = htonl(sender_ip);
      if (eth_tx(reply, arp->sha, ETHTYPE_ARP) < 0)
        mbuffree(reply);
    }
  }
  mbuffree(m);
}

// 第一版只发送无选项、无分片的 IPv4 包，整体不得超过 1500 字节 MTU。
int
ip_tx(struct mbuf *m, uint32 dst, uint8 proto)
{
  if (m->len + sizeof(struct ip) > ETH_MTU)
    return -1;
  struct ip *ip = mbufpush(m, sizeof(*ip));
  if (ip == 0)
    return -1;
  memset(ip, 0, sizeof(*ip));
  ip->vhl = 0x45;
  ip->len = htons(m->len);
  ip->id = htons(__sync_fetch_and_add(&ip_id, 1));
  ip->off = 0;
  ip->ttl = 64;
  ip->proto = proto;
  ip->src = htonl(local_ip);
  ip->dst = htonl(dst);
  ip->checksum = htons(inet_checksum(ip, sizeof(*ip)));

  // 广播地址直接使用广播 MAC；
  uint32 subnet_broadcast = local_ip | ~netmask;
  if (dst == 0xffffffffU || dst == subnet_broadcast)
    return eth_tx(m, broadcast_mac, ETHTYPE_IP);
  // 同网段直达，否则经默认网关解析 ARP。
  uint32 next_hop = ((dst & netmask) == (local_ip & netmask)) ? dst : gateway_ip;
  return arp_resolve_and_send(next_hop, m);
}

// ip rx 调用
// 只实现 Echo Request。复用收到的 mbuf 原地改成 Echo Reply 后发回。
static void
icmp_rx(struct mbuf *m, uint32 src)
{
  if (m->len < sizeof(struct icmp_echo)) {
    mbuffree(m);
    return;
  }
  struct icmp_echo *icmp = (struct icmp_echo *)m->head;
  if (icmp->type != ICMP_ECHO_REQUEST || icmp->code != 0 ||
      inet_checksum(icmp, m->len) != 0) {
    mbuffree(m);
    return;
  }
  icmp->type = ICMP_ECHO_REPLY;
  icmp->checksum = 0;
  icmp->checksum = htons(inet_checksum(icmp, m->len));
  if (ip_tx(m, src, IPPROTO_ICMP) < 0)
    mbuffree(m);
}

// eth rx 调用
// 仅接受固定 20 字节首部、完整未分片且目的地址属于本机的 IPv4 包。
static void
ip_rx(struct mbuf *m)
{
  if (m->len < sizeof(struct ip)) {
    mbuffree(m);
    return;
  }
  struct ip *ip = (struct ip *)m->head;
  if (ip->vhl != 0x45 || inet_checksum(ip, sizeof(*ip)) != 0) {
    mbuffree(m);
    return;
  }
  uint len = ntohs(ip->len);
  uint16 off = ntohs(ip->off);
  uint32 src = ntohl(ip->src);
  uint32 dst = ntohl(ip->dst);
  uint32 subnet_broadcast = local_ip | ~netmask;
  if (len < sizeof(*ip) || len > m->len || (off & 0x3fff) != 0 ||
      (dst != local_ip && dst != 0xffffffffU && dst != subnet_broadcast)) {
    mbuffree(m);
    return;
  }
  // 网卡可能交付 Ethernet 最小帧填充，按 IPv4 total length 裁剪。
  if (m->len > len)
    mbuftrim(m, m->len - len);
  uint8 proto = ip->proto;
  mbufpull(m, sizeof(*ip));
  if (proto == IPPROTO_UDP)
    udp_rx(m, src, dst);
  else if (proto == IPPROTO_ICMP)
    icmp_rx(m, src);
  else
    mbuffree(m);
}

// eth recv 调用
// E1000 上送入口：剥离 Ethernet 首部后按 EtherType 分派。
void
net_rx(struct mbuf *m)
{
  if (m->len < sizeof(struct eth)) {
    mbuffree(m);
    return;
  }
  struct eth *eth = (struct eth *)m->head;
  uint16 type = ntohs(eth->type);
  mbufpull(m, sizeof(*eth));
  if (type == ETHTYPE_ARP)
    arp_rx(m);
  else if (type == ETHTYPE_IP)
    ip_rx(m);
  else
    mbuffree(m);
}
