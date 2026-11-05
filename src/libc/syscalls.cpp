// Emulation of the Linux (ARM EABI) system calls that packers issue with a raw `svc #0` instead of calling libc.
// Each call is logged as "[sys] ..."; calls that are not implemented fail with ENOSYS, which is how new ones are found.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "libc/stdio_imports.h"
#include "libc/system_imports.h"
#include "runtime/runtime.h"

namespace {

// ARM EABI system call numbers (they differ from x86-64's).
enum Syscall : uint32_t {
  ARM_SYS_EXIT = 1,
  ARM_SYS_FORK = 2,
  ARM_SYS_READ = 3,
  ARM_SYS_WRITE = 4,
  ARM_SYS_OPEN = 5,
  ARM_SYS_CLOSE = 6,
  ARM_SYS_GETPID = 20,
  ARM_SYS_GETUID = 24,
  ARM_SYS_PTRACE = 26,
  ARM_SYS_ACCESS = 33,
  ARM_SYS_KILL = 37,
  ARM_SYS_IOCTL = 54,
  ARM_SYS_SIGACTION = 67,
  ARM_SYS_GETTIMEOFDAY = 78,
  ARM_SYS_READLINK = 85,
  ARM_SYS_MUNMAP = 91,
  ARM_SYS_CLONE = 120,
  ARM_SYS_MPROTECT = 125,
  ARM_SYS_NANOSLEEP = 162,
  ARM_SYS_PRCTL = 172,
  ARM_SYS_RT_SIGACTION = 174,
  ARM_SYS_RT_SIGPROCMASK = 175,
  ARM_SYS_SIGALTSTACK = 186,
  ARM_SYS_MMAP2 = 192,
  ARM_SYS_GETUID32 = 199,
  ARM_SYS_GETEUID32 = 201,
  ARM_SYS_MADVISE = 220,
  ARM_SYS_GETTID = 224,
  ARM_SYS_FUTEX = 240,
  ARM_SYS_EXIT_GROUP = 248,
  ARM_SYS_CLOCK_GETTIME = 263,
  ARM_SYS_TGKILL = 268,
  ARM_SYS_INOTIFY_INIT = 316,
  ARM_SYS_INOTIFY_ADD_WATCH = 317,
  ARM_SYS_INOTIFY_RM_WATCH = 318,
  ARM_SYS_OPENAT = 322,
};

constexpr int kProcessId = 4242;
constexpr int kUserId = 10123;
constexpr int kAtFdCwd = -100;          // the guest's AT_FDCWD
// Paths the guest opens may be redirected (see g_path_hook), for example to serve a synthetic /proc/self/maps.
const char* rewrite_path(const char* path) {
  return g_path_hook ? g_path_hook(path) : path;
}

constexpr int kOpenFlagMask = 0xFFFFF;  // ARM and x86-64 agree on the low open() flags we care about

long errno_result(long r) {
  return r < 0 ? -errno : r;
}

// mmap2(addr, len, prot, flags, fd, offset in 4 KiB pages)
long sys_mmap2(const uint32_t* a) {
  constexpr uint32_t MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20;
  uint32_t len = (a[1] + 4095) & ~4095u;
  uint32_t mem = (a[3] & MAP_FIXED) && a[0] ? a[0] : gmemalign(4096, len);
  memset(H(mem), 0, len);
  if ((int)a[4] >= 0 && !(a[3] & MAP_ANONYMOUS))
    (void)!pread(a[4], H(mem), len, (off_t)a[5] * 4096);
  return mem;
}

long sys_clock_gettime(const uint32_t* a) {  // the guest's timespec has two 32-bit fields
  timespec ts;
  clock_gettime(a[0], &ts);
  ((int32_t*)H(a[1]))[0] = (int32_t)ts.tv_sec;
  ((int32_t*)H(a[1]))[1] = (int32_t)ts.tv_nsec;
  return 0;
}

long sys_gettimeofday(const uint32_t* a) {
  timeval tv;
  gettimeofday(&tv, nullptr);
  if (a[0]) {
    ((int32_t*)H(a[0]))[0] = (int32_t)tv.tv_sec;
    ((int32_t*)H(a[0]))[1] = (int32_t)tv.tv_usec;
  }
  return 0;
}

// Returns the result of the system call `nr` with arguments a[0..5].
long dispatch(uint32_t nr, const uint32_t* a, const char** name) {
  switch (nr) {
    case ARM_SYS_EXIT:
    case ARM_SYS_EXIT_GROUP:
      *name = "exit";
      return 0;  // the guest is not allowed to end the process
    case ARM_SYS_READ:
      *name = "read";
      return errno_result(read(a[0], H(a[1]), a[2]));
    case ARM_SYS_WRITE:
      *name = "write";
      return errno_result(write(a[0], H(a[1]), a[2]));
    case ARM_SYS_OPEN:
      *name = "open";
      return errno_result(open(rewrite_path((const char*)H(a[0])), (int)a[1] & kOpenFlagMask, a[2]));
    case ARM_SYS_OPENAT:
      *name = "openat";
      return errno_result(
          openat((int)a[0] == kAtFdCwd ? AT_FDCWD : (int)a[0], rewrite_path((const char*)H(a[1])), (int)a[2] & kOpenFlagMask, a[3]));
    case ARM_SYS_CLOSE:
      *name = "close";
      return errno_result(close(a[0]));
    case ARM_SYS_GETPID:
    case ARM_SYS_GETTID:
      *name = "getpid/gettid";
      return kProcessId;
    case ARM_SYS_GETUID:
    case ARM_SYS_GETUID32:
    case ARM_SYS_GETEUID32:
      *name = "getuid";
      return kUserId;
    case ARM_SYS_ACCESS:
      *name = "access";
      return errno_result(access((const char*)H(a[0]), a[1]));
    case ARM_SYS_READLINK:
      *name = "readlink";
      return errno_result(readlink((const char*)H(a[0]), (char*)H(a[1]), a[2]));
    case ARM_SYS_MMAP2:
      *name = "mmap2";
      return sys_mmap2(a);
    case ARM_SYS_CLOCK_GETTIME:
      *name = "clock_gettime";
      return sys_clock_gettime(a);
    case ARM_SYS_GETTIMEOFDAY:
      *name = "gettimeofday";
      return sys_gettimeofday(a);
    case ARM_SYS_IOCTL:
      *name = "ioctl";
      return -ENOTTY;
    case ARM_SYS_PTRACE:
      *name = "ptrace";
      return -EPERM;  // nobody is tracing us and we cannot trace anyone
    case ARM_SYS_FORK:
    case ARM_SYS_CLONE:
    case ARM_SYS_INOTIFY_INIT:
    case ARM_SYS_INOTIFY_ADD_WATCH:
    case ARM_SYS_INOTIFY_RM_WATCH:
      *name = "fork/clone/inotify";
      return -ENOSYS;
    case ARM_SYS_KILL:
    case ARM_SYS_TGKILL:
    case ARM_SYS_PRCTL:
    case ARM_SYS_SIGACTION:
    case ARM_SYS_RT_SIGACTION:
    case ARM_SYS_RT_SIGPROCMASK:
    case ARM_SYS_SIGALTSTACK:
    case ARM_SYS_MUNMAP:
    case ARM_SYS_MPROTECT:
    case ARM_SYS_MADVISE:
    case ARM_SYS_NANOSLEEP:
    case ARM_SYS_FUTEX:
      *name = "ignored";
      return 0;
    default:
      *name = "UNIMPLEMENTED";
      return -ENOSYS;
  }
}

void syscall_hook(uint32_t* regs) {
  uint32_t nr = regs[7];
  const uint32_t* args = regs;  // r0-r5
  const char* name = "?";
  long result = dispatch(nr, args, &name);
  LOG("[sys] #%u %s(%08x, %08x, %08x, %08x) = %ld  lr=%08x", nr, name, args[0], args[1], args[2], args[3], result,
      regs[14]);
  regs[0] = (uint32_t)result;
}

}  // namespace

void install_syscalls() {
  g_syscall_hook = syscall_hook;
}
