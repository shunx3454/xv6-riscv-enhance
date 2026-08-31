# xv6-netif-guide.md

# xv6 简化网络协议栈与 Socket 接口实现指南

## 1. 实现目标与边界

基于已经完成的 E1000 驱动，继续实现：

```text
User
 |
socket / bind / connect
sendto / recvfrom
read / write / close / dup / fork
 |
struct file (FD_SOCKET)
 |
struct socket
 |
UDP
 |
IPv4
 |\
 | ARP
 | ICMP Echo
 |
Ethernet
 |
E1000
```

第一版建议限制为：

- 只支持 `AF_INET + SOCK_DGRAM + UDP`
- 单网卡、单 IPv4 地址
- 固定 netmask / gateway
- IPv4 无 option、无 fragmentation
- ICMP 只实现内核自动响应 Echo Request
- UDP payload 最大 1472 字节
- 普通 Ethernet MTU 1500
- 一个 packet = 一个 2048-byte mbuf = 一个 E1000 descriptor
- 一个 local UDP port 最多绑定一个 socket
- UDP RX queue 有固定上限
- `sendto/recvfrom` 的 flags 第一版只接受 0

核心目标不是复制完整 BSD socket，而是把 xv6 已有的：

```text
fd
struct file
ref count
dup/fork/close
sleep/wakeup
copyin/copyout
spinlock
interrupt
mbuf
```

和网络协议栈完整串起来。

---

# 2. 与 E1000 层的边界

E1000 向上只暴露两个核心方向：

```text
TX:
完整 Ethernet frame mbuf
        |
        v
e1000_transmit(m)

RX:
E1000 DMA
        |
完整 Ethernet frame mbuf
        |
        v
net_rx(m)
```

协议栈不再关心 `TDH/TDT/RDH/RDT`、descriptor DD、TXDW/RXDW 等细节。

---

# 3. mbuf 与 MTU 简化

普通 Ethernet：

```text
IPv4 MTU             1500
IPv4 header            20
UDP header               8
--------------------------
最大 UDP payload       1472
```

因此 2048-byte mbuf 足够：

```text
Ethernet 14
IPv4     20
UDP       8
payload <= 1472
```

所以可以坚持：

```text
1 packet
=
1 mbuf
=
1 TX/RX descriptor
```

这让整个协议栈都无需 scatter/gather 和 IP fragmentation。

建议保留 headroom：

```c
#define NET_HEADROOM 128
```

发送时：

```text
payload
  |
push UDP
  |
push IPv4
  |
push Ethernet
```

接收时相反：

```text
pull Ethernet
pull IPv4
pull UDP
```

---

# 4. mbuf helper

推荐：

```c
void *mbufpush(struct mbuf *m, uint n);
void *mbufpull(struct mbuf *m, uint n);
```

例如：

```c
void *
mbufpush(struct mbuf *m, uint n)
{
  if(m->head - n < m->buf)
    panic("mbufpush");

  m->head -= n;
  m->len += n;
  return m->head;
}
```

```c
void *
mbufpull(struct mbuf *m, uint n)
{
  if(m->len < n)
    return 0;

  void *p = m->head;
  m->head += n;
  m->len -= n;
  return p;
}
```

---

# 5. 网络字节序

RISC-V xv6 是 little-endian，而网络协议字段使用 big-endian。

需要：

```c
htons()
ntohs()
htonl()
ntohl()
```

原则：

```text
内核内部 IP/port:
host byte order

写入 packet:
network byte order

从 packet 读取:
ntoh*
```

---

# 6. 网络接口配置

第一版可静态配置：

```c
struct net_config {
  uchar mac[6];
  uint32 ip;
  uint32 netmask;
  uint32 gateway;
};
```

发送时要区分：

```text
dst 与 local 同子网
    -> ARP dst

dst 不同子网
    -> ARP gateway
```

但 IPv4 header 的 `dst` 始终是最终目标 IP。

---

# 7. Ethernet 层

Header：

```c
struct eth {
  uchar dst[6];
  uchar src[6];
  uint16 type;
} __attribute__((packed));
```

EtherType：

```c
#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806
```

RX：

```text
net_rx()
 |
pull Ethernet
 |
EtherType
 +--> ARP
 +--> IPv4
```

TX：

```text
eth_tx(m, dst_mac, type)
 |
push Ethernet header
 |
e1000_transmit(m)
```

必须明确 ownership：

```text
e1000_transmit success
    -> E1000 owns mbuf

e1000_transmit failure
    -> caller still owns mbuf
```

---

# 8. ARP 的职责

ARP 负责：

```text
next-hop IPv4
     |
     v
MAC address
```

不是为最终远端 IP 一定直接找 MAC。

跨网段：

```text
IP dst      = remote host
Ethernet dst = gateway MAC
```

---

# 9. ARP Header

```c
struct arp {
  uint16 hrd;
  uint16 pro;
  uint8  hln;
  uint8  pln;
  uint16 op;

  uchar  sha[6];
  uint32 spa;

  uchar  tha[6];
  uint32 tpa;
} __attribute__((packed));
```

常量：

```text
hrd = 1
pro = IPv4
hln = 6
pln = 4

op = 1 request
op = 2 reply
```

---

# 10. ARP Cache

推荐：

```c
#define NARP 16
#define ARP_PENDING_MAX 8

enum arp_state {
  ARP_EMPTY,
  ARP_INCOMPLETE,
  ARP_REACHABLE,
};

struct arp_entry {
  enum arp_state state;

  uint32 ip;
  uchar mac[6];

  struct mbuf *pending_head;
  struct mbuf *pending_tail;
  int pending_count;
};
```

全局：

```c
struct {
  struct spinlock lock;
  struct arp_entry entry[NARP];
} arptable;
```

---

# 11. ARP Request / Reply

ARP miss：

```text
Ethernet dst = ff:ff:ff:ff:ff:ff

ARP:
op  = REQUEST
sha = local MAC
spa = local IP
tha = 00:00:00:00:00:00
tpa = wanted next-hop IP
```

收到 request：

```text
if target IP == local IP
    -> send ARP Reply
```

Reply：

```text
sha = local MAC
spa = local IP
tha = requester MAC
tpa = requester IP
```

ARP RX 中也可以顺手学习：

```text
sender IP -> sender MAC
```

---

# 12. ARP Pending Queue

这是发送路径最关键的地方之一。

IP packet 已经构造好：

```text
[IPv4][UDP][payload]
```

但 ARP miss 时，不应 spin 等待，也不应立即丢包。

应：

```text
ARP entry = INCOMPLETE
 |
pending queue owns mbuf
 |
send ARP request
```

收到 reply：

```text
update MAC
INCOMPLETE -> REACHABLE
 |
detach pending list
 |
release arp lock
 |
for each pending packet:
    eth_tx(packet, resolved_mac, IPv4)
```

不要拿着 `arp_lock` 调 `eth_tx()`。

---

# 13. IPv4 Header

第一版固定 20 bytes：

```c
struct ip {
  uint8  vhl;
  uint8  tos;
  uint16 len;
  uint16 id;
  uint16 off;
  uint8  ttl;
  uint8  proto;
  uint16 checksum;
  uint32 src;
  uint32 dst;
} __attribute__((packed));
```

协议号：

```c
#define IPPROTO_ICMP 1
#define IPPROTO_UDP 17
```

只支持：

```text
version = 4
IHL = 5
fragment = 0
TTL = 64
```

---

# 14. IPv4 checksum

IPv4 checksum 只覆盖 IP header。

可实现通用 one's-complement checksum：

```c
uint16 inet_checksum(void *buf, int len);
```

发送：

```c
ip->checksum = 0;
ip->checksum = inet_checksum(ip, sizeof(*ip));
```

---

# 15. IPv4 TX

接口：

```c
int ip_tx(struct mbuf *m, uint32 dst_ip, uint8 proto);
```

进入时：

```text
m -> [UDP][payload]
```

执行：

```text
push IPv4 header
 |
填 src/dst/len/ttl/proto
 |
算 checksum
 |
选择 next hop
 |
ARP lookup
```

ARP hit：

```text
eth_tx(m, mac, ETHTYPE_IP)
```

ARP miss：

```text
ARP pending queue 接管 m
ARP request
```

---

# 16. IPv4 RX

`net_rx()` 已 pull Ethernet 后：

```text
m->head -> IPv4
```

至少检查：

```text
version == 4
IHL == 5
total length >= 20
total length <= RX data length
destination 是本机
header checksum 正确
不支持 fragment
```

然后按 `proto` 分发：

```text
1  -> ICMP
17 -> UDP
```

注意应根据 `ip->len` trim 数据，不能把 Ethernet padding 当成 UDP payload。

---

# 17. ICMP：只做 Echo Request 自动回复

不增加 syscall，不做 raw socket。

路径：

```text
ip_rx
 |
proto == ICMP
 |
icmp_rx
 |
Echo Request?
 |
yes
 |
Echo Reply
```

Header：

```c
struct icmp_echo {
  uint8 type;
  uint8 code;
  uint16 checksum;
  uint16 id;
  uint16 seq;
} __attribute__((packed));
```

```text
Echo Request type = 8
Echo Reply   type = 0
code = 0
```

---

# 18. ICMP Echo Reply 最简单实现

进入 `icmp_rx()` 时 IPv4 header 已 pull 掉：

```text
m -> [ICMP request][payload]
```

可以直接复用原 RX mbuf：

```text
type 8 -> 0
checksum = 0
重新计算整个 ICMP message checksum
 |
ip_tx(m, original_src_ip, IPPROTO_ICMP)
```

这样：

```text
RX mbuf
  |
直接转换成 TX mbuf
```

无需重新分配或复制 payload。

如果不是 Echo Request：

```text
mbuffree(m)
```

---

# 19. UDP Header

```c
struct udp {
  uint16 sport;
  uint16 dport;
  uint16 len;
  uint16 checksum;
} __attribute__((packed));
```

第一版 IPv4 UDP：

```text
checksum = 0
```

即可。

---

# 20. UDP TX

接口：

```c
int udp_tx(struct mbuf *m,
           uint16 sport,
           uint32 dst_ip,
           uint16 dport);
```

进入：

```text
m -> payload
```

执行：

```text
push UDP
 |
sport
dport
len
checksum = 0
 |
ip_tx(..., IPPROTO_UDP)
```

最大 payload：

```c
#define UDP_MAX_PAYLOAD 1472
```

---

# 21. UDP RX

进入时 IPv4 header 已 pull：

```text
m -> [UDP][payload]
```

验证：

```text
m->len >= 8
udp_len >= 8
udp_len <= m->len
```

取：

```text
src IP
dst IP
src port
dst port
```

pull UDP header 后：

```text
m->head -> payload
m->len  -> UDP payload length
```

然后按 destination port 找 socket。

---

# 22. `struct file` 增加 `FD_SOCKET`

让 socket 真正融入 xv6 Unix fd 抽象：

```c
struct socket;

struct file {
  enum {
    FD_NONE,
    FD_PIPE,
    FD_INODE,
    FD_DEVICE,
    FD_SOCKET,
  } type;

  int ref;
  char readable;
  char writable;

  struct pipe *pipe;
  struct inode *ip;
  struct socket *sock;

  uint off;
  short major;
};
```

这样可以直接复用：

```text
read
write
close
dup
fork
```

---

# 23. `struct socket`

第一版：

```c
struct socket {
  int used;

  int bound;
  uint32 local_ip;
  uint16 local_port;

  int connected;
  uint32 remote_ip;
  uint16 remote_port;

  struct mbuf *rx_head;
  struct mbuf *rx_tail;
  int rx_count;
};
```

含义：

```text
local endpoint
optional default peer
UDP datagram RX queue
```

---

# 24. Socket Table

```c
#define NSOCKET 32
#define UDP_RXQ_MAX 16

struct {
  struct spinlock lock;
  struct socket sock[NSOCKET];
} socktable;
```

第一版用一把全局 socket lock 足够。

它保护：

```text
socket allocation
bind state
port ownership
connected peer
RX queue
rx_count
```

---

# 25. 建议新增 syscall

新增：

```c
socket()
bind()
connect()
sendto()
recvfrom()
```

其中 `connect()` 对 UDP 不发送握手，只设置默认 peer。

不新增：

```text
read
write
close
dup
fork
```

这些复用已有 syscall。

---

# 26. `socket()`

只支持：

```text
AF_INET
SOCK_DGRAM
protocol 0 / UDP
```

流程：

```text
socketalloc
 |
filealloc
 |
fdalloc
 |
f->type = FD_SOCKET
f->sock = s
f->readable = 1
f->writable = 1
 |
return fd
```

---

# 27. `bind()`

用户：

```c
bind(fd, &addr, sizeof(addr));
```

最小 sockaddr：

```c
struct sockaddr_in {
  uint16 sin_family;
  uint16 sin_port;
  uint32 sin_addr;
};
```

流程：

```text
argfd
 |
确认 FD_SOCKET
 |
copyin sockaddr
 |
检查 AF_INET
 |
检查 port 是否已占用
 |
记录 local_ip/local_port
```

第一版不实现：

```text
SO_REUSEADDR
SO_REUSEPORT
```

因此一个 local port 只能有一个 socket。

---

# 28. 自动 ephemeral port

如果客户端：

```c
fd = socket(...);
sendto(fd, ...);
```

没有显式 bind，`sendto()` 可自动：

```text
在 49152~65535 中找空闲 port
 |
绑定本地 IP:port
```

这是 `socket_autobind()`。

服务器仍显式：

```c
bind(fd, port)
```

---

# 29. `connect()` 对 UDP 的意义

只做：

```c
s->remote_ip = ...
s->remote_port = ...
s->connected = 1;
```

没有：

```text
SYN
SYN-ACK
ACK
```

它让：

```c
write(fd, buf, n)
```

有默认 destination。

也可以让 connected socket 只接受指定 peer 的 UDP packet。

---

# 30. `sendto()`

路径：

```text
sys_sendto
 |
argfd
 |
copyin destination sockaddr
 |
socket_sendto
 |
if unbound -> autobind
 |
allocate mbuf(NET_HEADROOM)
 |
copyin user payload
 |
udp_tx
 |
ip_tx
 |
ARP
 |
Ethernet
 |
E1000
```

要求：

```text
len <= 1472
flags == 0
```

---

# 31. `sendto()` 的返回时机

推荐：

> datagram 被内核网络栈接受后立即返回。

不等待：

```text
ARP reply
E1000 TXDW
远端收到
远端应用 recv
```

如果 ARP miss，但 packet 已成功放入 ARP pending queue：

```text
sendto return len
```

---

# 32. `write()` 复用

在 `filewrite()`：

```c
if(f->type == FD_SOCKET)
  return socketwrite(f->sock, addr, n);
```

`socketwrite()`：

```text
if !connected
    return -1

socket_sendto(
    default remote IP,
    default remote port
)
```

所以：

```text
write(fd, buf, n)
=
sendto(fd, buf, n, connected_peer)
```

---

# 33. UDP RX socket lookup

收到：

```text
src = A:sport
dst = local:dport
```

第一版匹配：

```text
used
bound
local_port == dport
local_ip == 0 or local_ip == dst_ip
```

如果 socket 已 connected，再要求：

```text
remote_ip == src_ip
remote_port == sport
```

没有匹配：

```text
drop mbuf
```

第一版不发 ICMP Port Unreachable。

---

# 34. Socket RX Queue

直接复用：

```c
m->next
```

形成：

```text
rx_head
  |
  v
packet A -> packet B -> packet C
                         ^
                         |
                      rx_tail
```

这意味着：

> E1000 DMA 得到的原 mbuf，在 Ethernet/IP/UDP 去头后直接进入 socket queue。

无需再复制一份 kernel buffer。

---

# 35. mbuf 保存 recvfrom metadata

可扩展：

```c
uint32 src_ip;
uint16 src_port;
```

在 `udp_rx()`：

```c
m->src_ip = src_ip;
m->src_port = sport;
```

进入 socket queue 时：

```text
metadata:
src IP / port

data:
m->head -> UDP payload
m->len  -> payload length
```

---

# 36. UDP RX Queue 上限

必须限制：

```c
#define UDP_RXQ_MAX 16
```

队列满：

```text
drop new datagram
mbuffree(m)
```

防止一个 port 吃光所有 mbuf。

---

# 37. `recvfrom()` 阻塞模型

核心和 xv6 pipe 一样：

```c
acquire(&socktable.lock);

while(s->rx_head == 0){
  if(killed(myproc())){
    release(&socktable.lock);
    return -1;
  }

  sleep(s, &socktable.lock);
}
```

不能用：

```c
if(empty) sleep()
```

必须用 `while`，因为 wakeup 后条件可能再次被别的进程改变。

---

# 38. RX interrupt 与 wakeup

完整链路：

```text
E1000 RX interrupt
 |
e1000_recv
 |
net_rx
 |
ip_rx
 |
udp_rx
 |
lookup socket
 |
enqueue mbuf
 |
wakeup(socket)
```

被阻塞的：

```text
recvfrom()
```

由 scheduler 重新变为 RUNNABLE。

中断路径绝不能等待用户进程。

---

# 39. `recvfrom()` dequeue

有数据：

```c
m = s->rx_head;
s->rx_head = m->next;

if(s->rx_head == 0)
  s->rx_tail = 0;

s->rx_count--;
```

然后：

```text
release socket lock
```

再做 `copyout()`。

不要拿全局 socket lock 长时间 copyout。

---

# 40. `recvfrom()` copyout

payload：

```c
nr = min(user_len, m->len);
copyout(..., user_buf, m->head, nr);
```

来源地址：

```text
m->src_ip
m->src_port
```

构造 sockaddr 后 copyout。

最后：

```c
mbuffree(m);
return nr;
```

---

# 41. UDP datagram boundary

必须保持：

```text
一次 read/recvfrom
=
最多一个 UDP datagram
```

例如：

```text
packet A = 100
packet B = 200

read(..., 150)
```

返回：

```text
100 bytes of A
```

不能：

```text
A 100 + B 50
```

UDP 不是 stream。

---

# 42. 用户 buffer 太小时

datagram：

```text
1000 bytes
```

用户：

```text
recv(..., 100)
```

第一版可以：

```text
copy first 100
drop remaining 900
free whole datagram
return 100
```

下一次 receive 读取下一个 datagram。

---

# 43. `read()` 复用

在 `fileread()`：

```c
if(f->type == FD_SOCKET)
  return socketread(f->sock, addr, n);
```

而：

```text
socketread()
=
socket_recvfrom(..., 不返回 source address)
```

所以：

```text
read(fd, buf, n)
=
recvfrom(fd, buf, n, ..., NULL)
```

---

# 44. `close()` 复用

`fileclose()` 最后一个引用释放时：

```c
if(ff.type == FD_SOCKET)
  socketclose(ff.sock);
```

`socketclose()`：

```text
移除 bind
标记 closed/unused
detach RX queue
wakeup sleepers
释放 queued mbuf
```

注意不要在锁内逐个做复杂跨层操作。

---

# 45. `dup()` 和 `fork()` 自动得到正确语义

`dup(fd)`：

```text
fd1 ---+
       |
       v
   same struct file
       |
       v
   same socket
       ^
       |
fd2 ---+
```

因此共享：

```text
local port
peer
RX queue
```

`fork()` 后父子进程同样通过同一个 `struct file` 引用同一个 socket。

如果两个进程同时 recv：

```text
一个 datagram 只会被其中一个取走
```

---

# 46. File offset

`FD_SOCKET` 不使用：

```c
f->off
```

socket 的 read/write 不改变 file offset。

---

# 47. Lock 分层

推荐至少：

```text
e1000_lock
arp_lock
socktable.lock
```

原则：

```text
本层改状态时持本层锁
跨层调用前尽量释放本层锁
```

特别避免：

```text
e1000_lock -> net_rx
arp_lock   -> eth_tx/e1000
socket lock -> ip_tx
```

---

# 48. ARP flush 的锁技巧

收到 ARP Reply：

```text
acquire arp lock
 |
更新 entry
detach pending list
 |
release arp lock
 |
for each packet:
    eth_tx()
```

而不是：

```text
hold arp lock
 |
eth_tx
 |
e1000 lock
```

减少死锁风险。

---

# 49. mbuf ownership：TX

```text
user buffer
 |
copyin
 |
socket owns mbuf
 |
UDP
 |
IP
 |
+-- ARP hit --------> Ethernet -> E1000 owns
|
+-- ARP miss -------> ARP pending owns
                         |
                      ARP reply
                         |
                      Ethernet
                         |
                      E1000 owns
```

最终由 TX completion 回收。

---

# 50. mbuf ownership：RX

```text
E1000 RX descriptor owns empty mbuf
 |
DMA
 |
e1000_recv replaces descriptor buffer
 |
old mbuf -> network stack owns
 |
+-- ICMP Echo
|      |
|      v
|   reuse same mbuf as TX
|
+-- UDP
       |
       v
   socket RX queue owns
       |
   recvfrom/read
       |
   copyout
       |
   mbuffree
```

最重要的不变量：

> 任意时刻，一个 mbuf 必须恰好有一个 owner。

---

# 51. 建议的模块接口

Ethernet：

```c
void net_rx(struct mbuf *);
int eth_tx(struct mbuf *, uchar dst[6], uint16 type);
```

ARP：

```c
int  arp_lookup(uint32, uchar mac[6]);
void arp_request(uint32);
void arp_rx(struct mbuf *);
int  arp_queue(uint32 next_hop, struct mbuf *);
```

IPv4：

```c
int  ip_tx(struct mbuf *, uint32 dst, uint8 proto);
void ip_rx(struct mbuf *);
```

ICMP：

```c
void icmp_rx(struct mbuf *, uint32 src, uint32 dst);
```

UDP：

```c
int  udp_tx(struct mbuf *, uint16 sport, uint32 dst, uint16 dport);
void udp_rx(struct mbuf *, uint32 src, uint32 dst);
```

Socket：

```c
struct socket *socketalloc(void);
void socketclose(struct socket *);

int socket_bind(...);
int socket_connect(...);
int socket_sendto(...);
int socket_recvfrom(...);

int socketread(...);
int socketwrite(...);
```

---

# 52. 推荐文件组织

```text
kernel/e1000.c
    E1000

kernel/net.c
    Ethernet
    ARP
    IPv4
    ICMP

kernel/udp.c
    UDP tx/rx

kernel/socket.c
    socket object
    bind/connect
    RX queue
    sendto/recvfrom
    read/write backend

kernel/file.c
    FD_SOCKET dispatch

kernel/sysfile.c or syssock.c
    socket syscalls

kernel/net.h
kernel/socket.h
```

第一版也可以减少文件数量，重点是层次职责不要混。

---

# 53. 推荐开发顺序

## Phase 1：Ethernet + ARP

实现：

```text
net_rx
eth_tx
ARP request/reply/cache
ARP pending queue
```

## Phase 2：IPv4 + ICMP Echo

实现：

```text
IPv4 TX/RX
IP checksum
next-hop
ICMP Echo Reply
```

测试：

```text
宿主机 ping xv6
```

## Phase 3：UDP 内核层

实现：

```text
udp_tx
udp_rx
port demux hook
```

## Phase 4：FD_SOCKET 生命周期

实现：

```text
socket()
FD_SOCKET
fileclose
dup/fork compatibility
```

## Phase 5：bind + recvfrom

实现：

```text
bind namespace
socket RX queue
sleep/wakeup
copyout
```

## Phase 6：sendto

实现：

```text
autobind
copyin
UDP/IP/ARP/Ethernet TX
```

## Phase 7：connect + read/write

实现：

```text
connected peer
write -> sendto
read -> recvfrom
```

---

# 54. 典型测试程序：UDP Echo Server

```text
socket
 |
bind(:1234)
 |
recvfrom
 |
sleep
 |
packet arrives
 |
wakeup
 |
recvfrom returns
 |
sendto(source)
```

它一次测试：

```text
socket fd
bind
UDP demux
RX queue
sleep/wakeup
recvfrom metadata
sendto
ARP
IPv4
E1000
```

---

# 55. 典型测试：connected UDP

```c
fd = socket(AF_INET, SOCK_DGRAM, 0);
connect(fd, &server, sizeof(server));

write(fd, "hello", 5);
n = read(fd, buf, sizeof(buf));
```

验证：

```text
connect
filewrite -> socketwrite -> sendto
fileread -> socketread -> recvfrom
```

---

# 56. 常见实现错误

### 错误 1：给远端跨网段 IP 直接 ARP

正确：

```text
ARP gateway
IPv4 dst 仍为远端
```

### 错误 2：ARP miss 时阻塞 spin

正确：

```text
pending queue + ARP request
```

### 错误 3：把 UDP 当 byte stream

正确：

```text
一次 receive 最多一个 datagram
```

### 错误 4：在 socket lock 内 copyout

正确：

```text
dequeue
unlock
copyout
```

### 错误 5：持 e1000_lock 调 net_rx

可能因 ARP reply/UDP reply 再次进入 TX 而死锁。

### 错误 6：ICMP type 改完不重算 checksum

Echo Reply 必须重新计算 ICMP checksum。

### 错误 7：忽略 IP total length

必须 trim Ethernet padding。

### 错误 8：ARP pending / socket RX queue 无上限

会耗光 mbuf。

### 错误 9：多个层都 free 同一个 mbuf

必须严格遵守 ownership transfer。

---

# 57. 最终完整 TX 链路

```text
User
 |
sendto / write
 |
struct file FD_SOCKET
 |
struct socket
 |
copyin
 |
mbuf payload
 |
UDP header
 |
IPv4 header
 |
ARP next-hop resolution
 |
Ethernet header
 |
E1000 TX descriptor EOP|RS
 |
DMA
 |
TXDW completion
 |
mbuffree
```

---

# 58. 最终完整 RX 链路

```text
Ethernet wire
 |
E1000 DMA
 |
RX descriptor DD/EOP
 |
e1000_recv
 |
net_rx
 |
EtherType
 +--> ARP request/reply/cache
 |
 +--> IPv4
        |
        +--> ICMP Echo Request
        |      |
        |      -> Echo Reply
        |
        +--> UDP
               |
            socket lookup
               |
            RX queue
               |
            wakeup
               |
       recvfrom / read
               |
            copyout
               |
             user
```

---

# 59. 最终架构图

```text
+--------------------------------------------------+
|                   User Process                   |
| socket bind connect sendto recvfrom read write  |
+--------------------------+-----------------------+
                           |
                           v
+--------------------------------------------------+
|                 struct file                      |
|                   FD_SOCKET                      |
|         ref / readable / writable                |
+--------------------------+-----------------------+
                           |
                           v
+--------------------------------------------------+
|                 struct socket                    |
| local ip:port                                    |
| remote ip:port                                   |
| RX datagram queue                                |
+--------------------+-----------------------------+
                     |
                     v
+--------------------------------------------------+
|                      UDP                         |
|          port header / socket demux              |
+--------------------+-----------------------------+
                     |
                     v
+--------------------------------------------------+
|                     IPv4                         |
|       checksum / next-hop / proto demux          |
+------------+-------------------------+-----------+
             |                         |
             v                         v
+------------------------+      +------------------+
|          ARP           |      |       ICMP       |
| cache / request/reply  |      |  Echo Request    |
| pending queue          |      |  Echo Reply      |
+------------+-----------+      +------------------+
             |
             v
+--------------------------------------------------+
|                   Ethernet                       |
| MAC header / EtherType                           |
+--------------------------+-----------------------+
                           |
                           v
+--------------------------------------------------+
|                     E1000                        |
| TX/RX ring / DMA / TXDW / RXDW                  |
+--------------------------------------------------+
```

---

# 60. 核心设计结论

这套简化网络栈最重要的不是协议字段本身，而是以下几个 OS 设计点：

1. `struct file` 管理 Unix fd 生命周期；`struct socket` 管理网络 endpoint。
2. ARP/IP/UDP 都是内核内部协议层，不需要为每层增加 syscall。
3. `sendto()` 把用户 payload copyin 到 mbuf 后，ownership 沿 UDP -> IP -> ARP/Ethernet -> E1000 单向转移。
4. ARP miss 时用 pending queue，而不是阻塞中断或 spin 等待。
5. RX packet 经过 Ethernet/IP/UDP 去头后，原 mbuf 直接进入 socket RX queue。
6. `recvfrom()` 用 `sleep/wakeup` 把中断生产 packet 和进程消费 packet 解耦。
7. UDP 必须保持 datagram boundary。
8. `read()` 复用 `recvfrom()`；`write()` 在 UDP `connect()` 后复用 `sendto()`。
9. `dup/fork/close` 通过 `struct file.ref` 自然获得共享 socket 的 Unix 语义。
10. ICMP Echo Request 可直接复用收到的 mbuf 改成 Echo Reply，再走 `ip_tx()` 发回。
11. 所有 queue 都必须有上限。
12. 任意时刻一个 mbuf 必须只有一个 owner。
13. 尽量不要跨协议层持锁，尤其不要拿 E1000/ARP/socket 锁跨层调用。

完成这些后，xv6 就具备了一套小型但完整的 Unix 风格 UDP 网络栈：

```text
fd/socket
   |
UDP
   |
IPv4
   |
ARP + ICMP
   |
Ethernet
   |
E1000
```

并且会把之前学习过的中断、DMA、spinlock、sleep/wakeup、调度、file 引用计数、fork/dup、copyin/copyout 和内核对象生命周期全部串在一起。

```
## 发送路径

  UDP 正常发送：

  用户态
    sendto() / write()
          │
          ▼
  系统调用层
    sys_sendto()
    或 sys_write() → filewrite()
          │
          ▼
  Socket 层
    socket_sendto()
          │
          ▼
  UDP 层
    udp_tx()
    压入 UDP 首部
          │
          ▼
  IPv4 层
    ip_tx()
    压入 IPv4 首部
    选择下一跳
          │
          ▼
  ARP 解析
          │
          ├─ ARP 命中
          │     ▼
          │   eth_tx()
          │
          └─ ARP 未命中
                │
                ├─ IPv4 mbuf 放入 ARP pending
                ├─ arp_request()
                └─ ARP 解析成功后 arp_learn() 冲刷 pending
                      │
                      ▼
  Ethernet 层
    eth_tx()
    压入 Ethernet 首部
          │
          ▼
  E1000 驱动
    e1000_transmit()
          │
          ▼
  TX DMA 描述符环
          │
          ▼
  虚拟网卡发送

  简化表示：

  用户 TX
   → socket TX
   → UDP TX
   → IPv4 TX
   → ARP 解析
   → Ethernet TX
   → E1000 TX

  ARP 本身不是经过 IPv4 发送的：

  arp_request()/ARP Reply
   → eth_tx()
   → e1000_transmit()

  所以不是：

  UDP → IP → ARP 报文

  而是：

  UDP → IP
            │
            └─ ARP 负责查询 IP 包应该使用的 Ethernet 目的 MAC

  ## 接收路径

  网卡接收：

  E1000 收到 Ethernet 帧
          │
          ▼
  PLIC IRQ 33
          │
          ▼
  e1000_intr()
          │
          ▼
  e1000_recv()
    从 RX 描述符取出 mbuf
          │
          ▼
  net_rx()
    剥离 Ethernet 首部
    根据 EtherType 分支
          │
          ├────────────── ARP ──────────────┐
          │                                  ▼
          │                              arp_rx()
          │                           学习映射/发送应答
          │
          └────────────── IPv4
                             │
                             ▼
                          ip_rx()
                      剥离 IPv4 首部
                             │
                  ┌──────────┴──────────┐
                  │                     │
                ICMP                   UDP
                  │                     │
                  ▼                     ▼
              icmp_rx()              udp_rx()
            Echo Request Reply      剥离 UDP 首部
                                        │
                                        ▼
                                socket_rx_udp()
                                 查找目标 socket
                                 mbuf 加入接收队列
                                        │
                                        ▼
                                   wakeup(socket)
                                        │
                                        ▼
                            socket_recvfrom()/read()
                                出队并 copyout
                                        │
                                        ▼
                                     用户 RX

  简化表示：

  E1000 RX 中断
   → E1000 RX
   → Ethernet RX
   → ARP 或 IPv4
                 │
                 ├─ ICMP
                 └─ UDP → socket RX → 用户 RX

  因此你原来的接收模型：

  eth interrupt
   → eth rx
   → (ip rx/icmp rx/arp rx)
   → udp rx
   → sock rx
   → 用户 rx

  更准确地应该写成：

  E1000 interrupt
   → E1000 RX
   → Ethernet RX
   → {
        ARP RX
        或
        IPv4 RX → {
                     ICMP RX
                     或
                     UDP RX → socket RX
                   }
      }

  ## 用户 RX 与中断的关系

  用户接收不是中断直接调用用户代码，而是生产者—消费者模型：

  用户进程执行 recvfrom()
          │
          ▼
  接收队列为空
          │
          ▼
  sleep(socket)

  随后：

  E1000 中断
   → UDP RX
   → socket 接收队列加入 mbuf
   → wakeup(socket)

  最后：

  进程被调度运行
   → recvfrom() 出队
   → copyout 到用户缓冲区
   → 返回用户态

  ## 对应当前函数名称

   抽象层      发送                         接收
  ━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   用户 API    sendto/write                 recvfrom/read
  ──────────  ───────────────────────────  ───────────────────────────────
   系统调用    sys_sendto/sys_write         sys_recvfrom/sys_read
  ──────────  ───────────────────────────  ───────────────────────────────
   Socket      socket_sendto/socketwrite    socket_rx_udp/socket_recvfrom
  ──────────  ───────────────────────────  ───────────────────────────────
   UDP         udp_tx                       udp_rx
  ──────────  ───────────────────────────  ───────────────────────────────
   IPv4        ip_tx                        ip_rx
  ──────────  ───────────────────────────  ───────────────────────────────
   ARP         arp_request/arp_learn        arp_rx
  ──────────  ───────────────────────────  ───────────────────────────────
   Ethernet    eth_tx                       net_rx 中的 EtherType 分派
  ──────────  ───────────────────────────  ───────────────────────────────
   E1000       e1000_transmit               e1000_intr/e1000_recv

  所以最终可以概括为：

  TX:
  用户 → fd → socket → UDP → IPv4 → ARP/Ethernet → E1000

  RX:
  E1000 → Ethernet → ARP 或 IPv4 → ICMP 或 UDP → socket 队列 → 用户

```
