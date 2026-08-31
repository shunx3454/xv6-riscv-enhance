#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "net.h"

// 发送路径进入时 m 只包含 payload；压入 UDP 首部后交给 IPv4 层。
/// @brief 
/// @param m 数据载体
/// @param sport 源端口
/// @param dst 目的地址
/// @param dport 目的端口
/// @return 
int
udp_tx(struct mbuf *m, uint16 sport, uint32 dst, uint16 dport)
{
  if (m->len > UDP_MAX_PAYLOAD)
    return -1;
    // head 前移，len 变大
  struct udp *udp = mbufpush(m, sizeof(*udp));
  if (udp == 0)
    return -1;
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->len = htons(m->len);
  // IPv4 中 UDP checksum 可以为 0，第一版暂不计算伪首部校验和。
  udp->checksum = 0;
  return ip_tx(m, dst, IPPROTO_UDP);
}

// 被 ip 层级调用
// 接收路径进入时 IPv4 首部已被移除，先验证 UDP 声明长度。
void
udp_rx(struct mbuf *m, uint32 src, uint32 dst)
{
  if (m->len < sizeof(struct udp)) {
    mbuffree(m);
    return;
  }
  struct udp *udp = (struct udp *)m->head;
  uint len = ntohs(udp->len);
  if (len < sizeof(*udp) || len > m->len) {
    mbuffree(m);
    return;
  }
  uint16 sport = ntohs(udp->sport);
  uint16 dport = ntohs(udp->dport);
  if (m->len > len)
    mbuftrim(m, m->len - len);
  mbufpull(m, sizeof(*udp));
  // 剥离 UDP 首部后把来源元数据留在 mbuf 中，供 recvfrom() 返回。
  m->src_ip = src;
  m->src_port = sport;
  socket_rx_udp(m, src, sport, dst, dport);
}
