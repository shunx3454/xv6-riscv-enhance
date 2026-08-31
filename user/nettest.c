#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"

// UDP 回显服务器：监听 2000 端口，用 recvfrom() 获得来源地址，并把
// 每个数据报原样 sendto() 回发送方。
static void
server(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in local = {
    .sin_family = AF_INET,
    .sin_port = htons(2000),
    .sin_addr = htonl(INADDR_ANY),
  };
  if (fd < 0 || bind(fd, &local, sizeof(local)) < 0) {
    printf("nettest: server setup failed\n");
    exit(1);
  }
  printf("nettest: UDP server listening on 2000\n");
  for (;;) {
    char buf[128];
    struct sockaddr_in peer;
    int peerlen = sizeof(peer);
    int n = recvfrom(fd, buf, sizeof(buf), 0, &peer, &peerlen);
    if (n < 0) {
      printf("nettest: recvfrom failed\n");
      exit(1);
    }
    if (sendto(fd, buf, n, 0, &peer, peerlen) != n) {
      printf("nettest: sendto failed\n");
      exit(1);
    }
    printf("nettest: echoed %d bytes\n", n);
  }
}

static void
client(int port)
{
  // 客户端连接 QEMU user-net 的宿主机地址 10.0.2.2，演示 connected
  // UDP socket 可以直接使用 write()/read() 收发一个完整数据报。
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in host = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr = htonl(MAKE_IP_ADDR(10, 0, 2, 2)),
  };
  char msg[] = "hello from xv6";
  char reply[128];
  if (fd < 0 || connect(fd, &host, sizeof(host)) < 0 ||
      write(fd, msg, sizeof(msg)) != sizeof(msg)) {
    printf("nettest: client setup/send failed\n");
    exit(1);
  }
  int n = read(fd, reply, sizeof(reply) - 1);
  if (n < 0) {
    printf("nettest: read failed\n");
    exit(1);
  }
  reply[n] = 0;
  printf("nettest: received %d bytes: %s\n", n, reply);
  close(fd);
}

int
main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "server") == 0)
    server();
  if (argc == 3 && strcmp(argv[1], "client") == 0) {
    client(atoi(argv[2]));
    exit(0);
  }
  printf("usage: nettest server | nettest client host-port\n");
  exit(1);
}
