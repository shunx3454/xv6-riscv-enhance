#ifndef XV6_SOCKET_H
#define XV6_SOCKET_H

// 固定大小的内核 socket 表；每个 socket 最多缓存 16 个 UDP 数据报。
#define NSOCKET     32
#define UDP_RXQ_MAX 16
#define EPHEMERAL_FIRST 49152
#define EPHEMERAL_LAST  65535

// 第一版仅实现 AF_INET/SOCK_DGRAM。所有 IP、端口字段在内核中均采用
// 主机字节序，只有复制 sockaddr 或组装协议首部时才转换字节序。
struct socket {
  int used;             // 表项是否已分配
  int bound;            // 是否已有本地地址和端口
  uint32 local_ip;
  uint16 local_port;
  int connected;        // UDP connect 只记录并过滤默认对端
  uint32 remote_ip;
  uint16 remote_port;
  struct mbuf *rx_head; // 保持数据报边界的 FIFO 接收队列
  struct mbuf *rx_tail;
  int rx_count;
};

#endif
