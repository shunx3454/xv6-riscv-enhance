#ifndef XV6_NET_H
#define XV6_NET_H

// 每个以太网包独占一个 mbuf。预留的头部空间用于依次压入
// UDP、IPv4 和 Ethernet 首部。
#define MBUF_SIZE    2048
#define NET_HEADROOM 128

#define ETHADDR_LEN 6
#define ETH_MTU     1500
#define UDP_MAX_PAYLOAD 1472

#define AF_INET    2
#define SOCK_DGRAM 2

#define INADDR_ANY 0

// 最小 IPv4 socket 地址。端口和地址在用户 ABI 中使用网络字节序。
struct sockaddr_in {
  uint16 sin_family;
  uint16 sin_port;
  uint32 sin_addr;
};

// 网络缓冲区及其所有权载体。next 既可连接 ARP 待发队列，也可连接
// socket 接收队列；src_ip/src_port 保存 recvfrom() 所需的来源信息。
struct mbuf {
  struct mbuf *next;
  char *head;
  uint len;
  uint32 src_ip;
  uint16 src_port;
  char buf[MBUF_SIZE];
};

void *mbufpush(struct mbuf *, uint);
void *mbufpull(struct mbuf *, uint);
void *mbufput(struct mbuf *, uint);
void *mbuftrim(struct mbuf *, uint);
struct mbuf *mbufalloc(uint);
void mbuffree(struct mbuf *);

// RISC-V 为小端，协议首部中的多字节字段使用大端网络字节序。
static inline uint16
bswap16(uint16 value)
{
  return (value << 8) | (value >> 8);
}

static inline uint32
bswap32(uint32 value)
{
  return ((value & 0x000000ffU) << 24) |
         ((value & 0x0000ff00U) << 8) |
         ((value & 0x00ff0000U) >> 8) |
         ((value & 0xff000000U) >> 24);
}

#define htons bswap16
#define ntohs bswap16
#define htonl bswap32
#define ntohl bswap32

#define MAKE_IP_ADDR(a, b, c, d)                                          \
  (((uint32)(a) << 24) | ((uint32)(b) << 16) | ((uint32)(c) << 8) |       \
   (uint32)(d))

// 以下协议首部必须与线上格式逐字节一致，禁止编译器插入填充。
struct eth {
  uint8 dst[ETHADDR_LEN];
  uint8 src[ETHADDR_LEN];
  uint16 type;
} __attribute__((packed));

#define ETHTYPE_IP  0x0800 // 以太网上层 IP 协议
#define ETHTYPE_ARP 0x0806 // 以太网上层 ARP 协议

struct arp {
  uint16 hrd;
  uint16 pro;
  uint8 hln;
  uint8 pln;
  uint16 op;
  uint8 sha[ETHADDR_LEN];
  uint32 spa;
  uint8 tha[ETHADDR_LEN];
  uint32 tpa;
} __attribute__((packed));

#define ARP_HRD_ETHER 1 // ETH 硬件类型

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct ip {
  uint8 vhl;
  uint8 tos;
  uint16 len;
  uint16 id;
  uint16 off;
  uint8 ttl;
  uint8 proto;
  uint16 checksum;
  uint32 src;
  uint32 dst;
} __attribute__((packed));

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17

struct icmp_echo {
  uint8 type;
  uint8 code;
  uint16 checksum;
  uint16 id;
  uint16 seq;
} __attribute__((packed));

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct udp {
  uint16 sport;
  uint16 dport;
  uint16 len;
  uint16 checksum;
} __attribute__((packed));

#endif
