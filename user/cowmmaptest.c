#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PGSIZE 4096
#define FILESIZE (2 * PGSIZE)

static void
fail(char *why)
{
  printf("cowmmaptest: %s\n", why);
  exit(1);
}

static void
makefile(char *path)
{
  char buf[512];
  int fd;

  memset(buf, 'A', sizeof(buf));
  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if (fd < 0)
    fail("create");
  for (int i = 0; i < FILESIZE; i += sizeof(buf))
    if (write(fd, buf, sizeof(buf)) != sizeof(buf))
      fail("seed write");
  close(fd);
}

static void
cowtest(void)
{
  char *p = sbrk(2 * PGSIZE);
  int status;

  if (p == SBRK_ERROR)
    fail("sbrk");
  p[0] = 'a';
  p[PGSIZE] = 'b';
  int pid = fork();
  if (pid < 0)
    fail("cow fork");
  if (pid == 0) {
    if (p[0] != 'a' || p[PGSIZE] != 'b')
      exit(1);
    p[0] = 'c';
    p[PGSIZE] = 'd';
    exit(p[0] == 'c' && p[PGSIZE] == 'd' ? 0 : 1);
  }
  if (wait(&status) != pid || status != 0)
    fail("cow child");
  if (p[0] != 'a' || p[PGSIZE] != 'b')
    fail("cow isolation");
}

static void
privatetest(char *path)
{
  int fd = open(path, O_RDONLY);
  int status;
  char *p;

  if (fd < 0)
    fail("private open");
  p = mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == (void *)-1)
    fail("private mmap");
  close(fd);
  if (p[0] != 'A' || p[PGSIZE] != 'A')
    fail("private contents");
  p[0] = 'P';
  int pid = fork();
  if (pid < 0)
    fail("private fork");
  if (pid == 0) {
    if (p[0] != 'P')
      exit(1);
    p[0] = 'C';
    exit(p[0] == 'C' ? 0 : 1);
  }
  if (wait(&status) != pid || status != 0)
    fail("private child");
  if (p[0] != 'P')
    fail("private fork isolation");
  if (munmap(p, FILESIZE) < 0)
    fail("private munmap");

  fd = open(path, O_RDONLY);
  char c;
  if (fd < 0 || read(fd, &c, 1) != 1 || c != 'A')
    fail("private wrote file");
  close(fd);
}

static void
sharedtest(char *path)
{
  int fd = open(path, O_RDWR);
  int status;
  char *p;

  if (fd < 0)
    fail("shared open");
  p = mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == (void *)-1)
    fail("shared mmap");
  p[0] = 'S';
  int pid = fork();
  if (pid < 0)
    fail("shared fork");
  if (pid == 0) {
    p[1] = 'T';
    exit(0);
  }
  if (wait(&status) != pid || status != 0 || p[1] != 'T')
    fail("shared visibility");
  if (munmap(p, FILESIZE) < 0)
    fail("shared munmap");
  close(fd);

  fd = open(path, O_RDONLY);
  char buf[2];
  if (fd < 0 || read(fd, buf, sizeof(buf)) != sizeof(buf) ||
      buf[0] != 'S' || buf[1] != 'T')
    fail("shared writeback");
  close(fd);
}

static void
protectiontest(char *path)
{
  int fd = open(path, O_RDONLY);
  int status;
  char *p;

  if (fd < 0)
    fail("readonly open");
  if (mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) !=
      (void *)-1)
    fail("writable shared on readonly fd");
  p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == (void *)-1 || p[0] != 'S')
    fail("readonly mmap");
  int pid = fork();
  if (pid < 0)
    fail("readonly fork");
  if (pid == 0) {
    p[0] = 'X';
    exit(0);
  }
  if (wait(&status) != pid || status != -1)
    fail("readonly protection");
  if (munmap(p, PGSIZE) < 0)
    fail("readonly munmap");
  close(fd);
}

static void
lifecycletest(char *path)
{
  int fd = open(path, O_RDWR);
  int status;

  if (fd < 0)
    fail("lifecycle open");
  int pid = fork();
  if (pid < 0)
    fail("exit fork");
  if (pid == 0) {
    char *p = mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == (void *)-1)
      exit(1);
    p[2] = 'E';
    close(fd);
    exit(0); // exit must write back and release the VMA.
  }
  if (wait(&status) != pid || status != 0)
    fail("exit cleanup");
  close(fd);

  fd = open(path, O_RDWR);
  if (fd < 0)
    fail("exec open");
  pid = fork();
  if (pid < 0)
    fail("exec fork");
  if (pid == 0) {
    char *p = mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *argv[] = {"echo", "mmap exec", 0};
    if (p == (void *)-1)
      exit(1);
    p[3] = 'X';
    close(fd);
    exec("echo", argv); // successful exec must write back the old VMA.
    exit(1);
  }
  if (wait(&status) != pid || status != 0)
    fail("exec cleanup");
  close(fd);

  fd = open(path, O_RDONLY);
  char buf[4];
  if (fd < 0 || read(fd, buf, sizeof(buf)) != sizeof(buf) ||
      memcmp(buf, "STEX", sizeof(buf)) != 0)
    fail("exit/exec writeback");
  close(fd);
}

static void
copytest(char *path)
{
  int fd = open(path, O_RDONLY);
  char *p;

  if (fd < 0)
    fail("copy open");
  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == (void *)-1)
    fail("copy mmap");
  if (read(fd, p, 2) != 2 || p[0] != 'S' || p[1] != 'T')
    fail("copyout mmap fault");
  if (write(1, p, 2) != 2)
    fail("copyin mmap");
  printf("\n");
  if (munmap(p, PGSIZE) < 0)
    fail("copy munmap");
  close(fd);
}

int
main(void)
{
  char *path = "cowmmap.data";

  cowtest();
  makefile(path);
  privatetest(path);
  sharedtest(path);
  lifecycletest(path);
  protectiontest(path);
  copytest(path);
  unlink(path);
  printf("cowmmaptest: OK\n");
  exit(0);
}
