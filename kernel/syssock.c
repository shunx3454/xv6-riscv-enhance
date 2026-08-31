#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// 从系统调用参数取得文件对象，并强制要求它是网络 socket。
static int
sockargfd(int n, struct file **pf)
{
  int fd;
  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (*pf = myproc()->ofile[fd]) == 0 ||
      (*pf)->type != FD_SOCKET)
    return -1;
  return 0;
}

static int
sockfdalloc(struct file *f)
{
  // 与 sysfile.c 的 fdalloc 规则一致：找到当前进程的第一个空槽。
  struct proc *p = myproc();
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

static int
fetch_sockaddr(uint64 addr, int len, struct sockaddr_in *sin)
{
  // sockaddr 来自不可信用户空间；复制固定大小并检查地址族。
  struct proc *p = myproc();
  if (addr == 0 || len < sizeof(*sin) ||
      copyin(p->pagetable, p->sz, (char *)sin, addr, sizeof(*sin)) < 0 ||
      sin->sin_family != AF_INET)
    return -1;
  return 0;
}

uint64
sys_socket(void)
{
  int domain, type, protocol;
  argint(0, &domain);
  argint(1, &type);
  argint(2, &protocol);
  // 当前协议族只支持 IPv4 UDP；protocol=0 表示选择默认 UDP 协议。
  if (domain != AF_INET || type != SOCK_DGRAM ||
      (protocol != 0 && protocol != IPPROTO_UDP))
    return -1;

  // socket、file 和 fd 三层必须全部分配成功，失败路径逐层回滚。
  // 使用固定表分配
  struct socket *s = socketalloc();
  if (s == 0)
    return -1;
    // ftable 固定分配表
  struct file *f = filealloc();
  if (f == 0) {
    socketclose(s);
    return -1;
  }
  // 进程级 文件fd 分配 最大16个
  int fd = sockfdalloc(f);
  if (fd < 0) {
    f->type = FD_NONE;
    fileclose(f);
    socketclose(s);
    return -1;
  }
  f->type = FD_SOCKET;
  f->readable = 1;
  f->writable = 1;
  // 把 file 与 sock 绑定
  f->socket = s;
  return fd;
}

uint64
sys_bind(void)
{
  struct file *f;
  uint64 addr;
  int len;
  struct sockaddr_in sin;
  argaddr(1, &addr);
  argint(2, &len);
  // 检查绑定的 fd 是否是 FD SOCK
  // 获取并检查 用户输入的 sin addr 是否和法
  if (sockargfd(0, &f) < 0 || fetch_sockaddr(addr, len, &sin) < 0)
    return -1;
  // 用户 ABI 使用网络字节序，socket 内部统一转换为主机字节序。
  return socket_bind(f->socket, ntohl(sin.sin_addr), ntohs(sin.sin_port));
}

uint64
sys_connect(void)
{
  struct file *f;
  uint64 addr;
  int len;
  struct sockaddr_in sin;
  argaddr(1, &addr);
  argint(2, &len);
  if (sockargfd(0, &f) < 0 || fetch_sockaddr(addr, len, &sin) < 0)
    return -1;
  return socket_connect(f->socket, ntohl(sin.sin_addr), ntohs(sin.sin_port));
}

uint64
sys_sendto(void)
{
  struct file *f;
  uint64 buf, dstaddr;
  int len, flags, dstlen;
  struct sockaddr_in sin;
  argaddr(1, &buf);
  argint(2, &len);
  argint(3, &flags);
  argaddr(4, &dstaddr);
  argint(5, &dstlen);
  if (sockargfd(0, &f) < 0 || flags != 0 || len < 0)
    return -1;
  // 目的地址为空时使用 connect() 保存的默认对端。
  if (dstaddr == 0)
    // sock，发送buf，发送len，0表示没有目的地址，0,0,
    return socket_sendto(f->socket, buf, len, 0, 0, 0);
  if (fetch_sockaddr(dstaddr, dstlen, &sin) < 0)
    return -1;
  return socket_sendto(f->socket, buf, len, 1, ntohl(sin.sin_addr),
                       ntohs(sin.sin_port));
}

uint64
sys_recvfrom(void)
{
  struct file *f;
  uint64 buf, srcaddr, addrlenp;
  int len, flags;
  argaddr(1, &buf);
  argint(2, &len);
  argint(3, &flags);
  argaddr(4, &srcaddr);
  argaddr(5, &addrlenp);
  // 第一版 flags 只能为 0；要求来源地址与长度指针成对出现。
  if (sockargfd(0, &f) < 0 || flags != 0 || len < 0 ||
      (srcaddr != 0 && addrlenp == 0))
    return -1;
    // sock，接受buf，buf len，对端地址，对端地址len
  return socket_recvfrom(f->socket, buf, len, srcaddr, addrlenp);
}
