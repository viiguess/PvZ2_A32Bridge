// Process, environment, dynamic-loader and math-classification imports.
#include <map>
#include <string>
#include "libc/misc_imports.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"
#include "runtime/jit.h"

// ------------------------------------------------------------------ allocator + misc handlers
// Guest heap is ~1.25 GB: report at most 1 GB of RAM so size-from-RAM caches stay allocatable.
static void import_sysconf(Ctx& ctx, const Stub&) {
  int name = (int)ctx.a(0);
  long value = sysconf(name);
  if (name == _SC_PHYS_PAGES || name == _SC_AVPHYS_PAGES) {
    long page_size = sysconf(_SC_PAGESIZE);
    long cap = (1L << 30) / page_size;
    if (value > cap)
      value = cap;
  }
  ctx.ret((u32)value);
}

static void return_zero(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void import_errno(Ctx& ctx, const Stub&) {
  ctx.ret(thread_errno_addr());
}

static void abort_process(Ctx& ctx, const Stub& stub) {
  LOG("guest %s() called, lr=%08x sp=%08x", stub.name, ctx.cpu->jit->Regs()[14], ctx.sp);
  // A32_ABORT_DUMP=<hexaddr>:<hexlen>[,...]: write those guest ranges to /tmp/a32_abort_<hexaddr>.bin (post-mortem)
  if (const char* spec = getenv("A32_ABORT_DUMP")) {
    std::string list = spec;
    for (size_t at = 0; at < list.size();) {
      size_t end = list.find(',', at);
      if (end == std::string::npos)
        end = list.size();
      unsigned long addr = 0, len = 0;
      if (sscanf(list.substr(at, end - at).c_str(), "%lx:%lx", &addr, &len) == 2) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/a32_abort_%lx.bin", addr);
        if (FILE* f = fopen(path, "wb")) {
          fwrite(H((u32)addr), 1, len, f);
          fclose(f);
        }
      }
      at = end + 1;
    }
  }
  abort();
}

static void exit_process(Ctx& ctx, const Stub&) {
  _exit(ctx.a(0));
}

static void import_assert2(Ctx& ctx, const Stub&) {
  LOG("guest assert failed: %s:%d %s: %s", (char*)ctx.p(0), ctx.a(1), (char*)ctx.p(2), (char*)ctx.p(3));
  abort();
}

static void import_dl_unwind_find_exidx(Ctx& ctx, const Stub&) {  // dl_unwind_find_exidx(pc, int* count)
  u32 pc = ctx.a(0);
  if (pc >= g_main.base && pc < g_main.base + g_main.size) {
    *(int*)ctx.p(1) = g_main.exidx_size / 8;
    ctx.ret(g_main.exidx);
  } else
    ctx.ret(0);
}

// Old packers ship their own linker: it walks bionic `soinfo` structs reached from the dlopen handle and does its own ELF
// hash lookup (strtab @0xac, symtab @0xb0, nbucket @0xb4, bucket @0xbc, chain @0xc0, base @0x8c, next @0xa4).
// So a handle is a fake soinfo whose tables export every symbol the bridge knows (absolute addresses, base 0).
static u32 build_fake_soinfo(const std::string& name) {
  static u32 strtab = 0, symtab = 0, buckets = 0, chains = 0, nbucket = 0, nsym = 0;
  if (!symtab) {
    std::vector<std::pair<std::string, u32>> syms = sym_all();
    nsym = (u32)syms.size() + 1;
    nbucket = 1021;
    size_t chars = 1;
    for (auto& sy : syms)
      chars += sy.first.size() + 1;
    strtab = gdata_alloc((u32)chars, 4);
    symtab = gdata_alloc(nsym * 16, 4);
    buckets = gdata_alloc(nbucket * 4, 4);
    chains = gdata_alloc(nsym * 4, 4);
    memset(H(strtab), 0, chars);
    memset(H(symtab), 0, nsym * 16);
    memset(H(buckets), 0, nbucket * 4);
    memset(H(chains), 0, nsym * 4);
    u32 at = 1, index = 1;
    for (auto& sy : syms) {
      memcpy((char*)H(strtab) + at, sy.first.c_str(), sy.first.size() + 1);
      u32* entry = (u32*)((char*)H(symtab) + index * 16);
      entry[0] = at;
      entry[1] = sy.second;
      entry[2] = 4;
      ((uint8_t*)entry)[12] = 0x12;  // GLOBAL FUNC
      ((uint16_t*)entry)[7] = 1;     // defined
      u32 h = 0;
      for (unsigned char c : sy.first) {
        h = (h << 4) + c;
        u32 g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
      }
      u32* bucket = (u32*)H(buckets) + h % nbucket;
      ((u32*)H(chains))[index] = *bucket;
      *bucket = index;
      at += (u32)sy.first.size() + 1;
      index++;
    }
  }
  u32 handle = gdata_alloc(0x200, 4);
  memset(H(handle), 0, 0x200);
  strncpy((char*)H(handle), name.c_str(), 127);
  u32* f = (u32*)H(handle);
  f[0xac / 4] = strtab;
  f[0xb0 / 4] = symtab;
  f[0xb4 / 4] = nbucket;
  f[0xb8 / 4] = nsym;
  f[0xbc / 4] = buckets;
  f[0xc0 / 4] = chains;
  return handle;
}

static void import_dlopen(Ctx& ctx, const Stub&) {
  static std::map<std::string, u32> handles;
  std::string name = ctx.a(0) ? (const char*)ctx.p(0) : "";
  size_t slash = name.rfind('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  u32& handle = handles[name];
  if (!handle)
    handle = build_fake_soinfo(name);
  ctx.ret(handle);
}

static void import_dlsym(Ctx& ctx, const Stub&) {
  const char* name = (const char*)ctx.p(1);
  u32 addr = stub_find(name);
  if (!addr)
    LOG("dlsym miss: %s", name);
  ctx.ret(addr);
}

static void import_isnan(Ctx& ctx, const Stub&) {
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  ctx.ret(std::isnan(number));
}

static void import_isinf(Ctx& ctx, const Stub&) {
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  ctx.ret(std::isinf(number) ? (number < 0 ? -1 : 1) : 0);
}

static void import_isfinite(Ctx& ctx, const Stub&) {
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  ctx.ret(std::isfinite(number));
}

static void import_fpclassifyd(Ctx& ctx, const Stub&) {
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  ctx.ret(std::fpclassify(number));
}

static void import_signbit(Ctx& ctx, const Stub&) {
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  ctx.ret(std::signbit(number) ? 1 : 0);
}

static void import_signbitf(Ctx& ctx, const Stub&) {
  u32 bits = ctx.a(0);
  float number;
  memcpy(&number, &bits, 4);
  ctx.ret(std::signbit(number) ? 1 : 0);
}

static void import_scalbnl(Ctx& ctx, const Stub&) {  // long double == double on arm32
  u64 bits = ctx.q(0);
  double number;
  memcpy(&number, &bits, 8);
  number = scalbn(number, (int)ctx.a(2));
  memcpy(&bits, &number, 8);
  ctx.ret64(bits);
}

static void import_getenv(Ctx& ctx, const Stub&) {
  const char* value = getenv((const char*)ctx.p(0));
  ctx.ret(value ? gstr(value) : 0);
}

static void import_strerror(Ctx& ctx, const Stub&) {
  ctx.ret(gstr(strerror((int)ctx.a(0))));
}

static void import_setlocale(Ctx& ctx, const Stub&) {
  ctx.ret(gstr("C"));
}

static void import_syscall(Ctx& ctx, const Stub&) {
  if (ctx.a(0) == 224)
    ctx.ret((u32)gettid());
  else if (ctx.a(0) == 0xf0002) {  // __ARM_NR_cacheflush(start, end, 0)
    cpu_invalidate_range(ctx.a(1), ctx.a(2) - ctx.a(1));
    ctx.ret(0);
  } else {
    errno = ENOSYS;
    set_errno();
    ctx.ret(-1);
  }
}

static void import_sigaction(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void return_minus_one(Ctx& ctx, const Stub&) {
  ctx.ret(-1);
}

static void import_gnu_Unwind_Find_exidx(Ctx& ctx, const Stub&) {
  if (ctx.a(1))
    *(u32*)ctx.p(1) = 0;
  ctx.ret(0);
}  // no unwind tables: exceptions unsupported

void register_misc_imports() {
  // math: arm32 softfp passes values in core registers; forward to the host libm
  for (const char* name :
       {"sin",   "cos",   "tan",   "atan",   "acos",   "asin",      "sqrt", "floor", "ceil", "log",  "log10",
        "exp",   "exp2",  "expm1", "log1p",  "logb",   "cbrt",      "erf",  "erfc",  "sinh", "cosh", "tanh",
        "asinh", "acosh", "atanh", "lgamma", "tgamma", "nearbyint", "rint", "fabs",  "round"})
    gen(name, "d>d");
  for (const char* name : {"atan2", "fmod", "pow", "hypot", "nextafter", "remainder"})
    gen(name, "dd>d");
  for (const char* name :
       {"sinf", "cosf", "tanf", "atanf", "acosf", "asinf", "sqrtf", "floorf", "ceilf", "truncf", "roundf", "log10f"})
    gen(name, "f>f");
  for (const char* name : {"atan2f", "fmodf", "powf", "nextafterf"})
    gen(name, "ff>f");
  gen("modf", "dp>d");
  gen("ldexp", "di>d");
  gen("frexp", "dp>d");
  gen("modff", "fp>f");
  gen("lroundf", "f>i");
  gen("lrint", "d>i");
  gen("llrint", "d>q");
  gen("remquo", "ddp>d");
  stub_add("scalbn", generic, dlsym(RTLD_DEFAULT, "scalbn"), "di>d");
  hle("scalbnl", import_scalbnl);
  hle("isnan", import_isnan);
  hle("__isinf", import_isinf);
  hle("__isfinite", import_isfinite);
  hle("__fpclassifyd", import_fpclassifyd);
  hle("__signbit", import_signbit);
  hle("__signbitf", import_signbitf);
  gen("lrand48", "v>i");
  gen("srand48", "i>v");

  // process, environment, dynamic loader
  {
    // `char** environ`: an empty environment. Packers resolve it by name and give up silently when it is missing.
    u32 empty_env = gdata_alloc(8, 4);
    memset(H(empty_env), 0, 8);
    u32 environ_var = gdata_alloc(4, 4);
    *(u32*)H(environ_var) = empty_env;
    sym_add("environ", environ_var);
  }
  // epoll: newer packers create one for a watcher thread. There is nothing to watch, so waits just time out.
  hle("epoll_create", [](Ctx& ctx, const Stub&) { ctx.ret((u32)open("/dev/null", O_RDONLY)); });
  hle("epoll_create1", [](Ctx& ctx, const Stub&) { ctx.ret((u32)open("/dev/null", O_RDONLY)); });
  hle("epoll_ctl", return_zero);
  hle("strsep", [](Ctx& ctx, const Stub&) {  // strsep(char** stringp, const char* delim) on guest memory
    u32* slot = (u32*)H(ctx.a(0));
    u32 start = *slot;
    if (!start) {
      ctx.ret(0);
      return;
    }
    char* text = (char*)H(start);
    char* hit = text + strcspn(text, (const char*)ctx.p(1));
    if (*hit) {
      *hit = 0;
      *slot = start + (u32)(hit + 1 - text);
    } else {
      *slot = 0;
    }
    ctx.ret(start);
  });
  hle("pipe", [](Ctx& ctx, const Stub&) {  // pipe(int fds[2]): old packers make one before forking a helper
    int fds[2];
    int rc = pipe(fds);
    if (rc == 0 && ctx.a(0)) {
      ((int*)H(ctx.a(0)))[0] = fds[0];
      ((int*)H(ctx.a(0)))[1] = fds[1];
    }
    ctx.ret((u32)rc);
  });
  hle("realpath", [](Ctx& ctx, const Stub&) {  // realpath(path, resolved): resolved may be null (malloc'd result)
    u32 out = ctx.a(1) ? ctx.a(1) : gmalloc(4096);
    char resolved[PATH_MAX];
    if (!realpath((const char*)ctx.p(0), resolved)) {
      ctx.ret(0);
      return;
    }
    strcpy((char*)H(out), resolved);
    ctx.ret(out);
  });
  for (const char* name : {"epoll_wait", "epoll_pwait"})
    hle(name, [](Ctx& ctx, const Stub&) {
      int timeout_ms = (int)ctx.a(3);
      usleep((unsigned)std::min(timeout_ms < 0 ? 50 : timeout_ms, 50) * 1000);
      ctx.ret(0);
    });
  hle("__errno", import_errno);
  hle("abort", abort_process);
  hle("exit", exit_process);
  hle("_exit", exit_process);
  hle("__assert2", import_assert2);
  hle("__stack_chk_fail", abort_process);
  for (const char* name : {"__cxa_atexit", "__cxa_finalize", "__aeabi_atexit", "raise", "getppid", "ptrace", "prctl",
                           "pthread_attr_destroy", "pthread_mutexattr_destroy", "pthread_mutexattr_setpshared",
                           "pthread_condattr_init", "pthread_condattr_destroy", "setpgid", "fcntl", "inet_ntoa", "popen"})
    hle(name, return_zero);
  for (const char* name : {"fork", "execv", "execve", "system", "waitpid", "creat", "pclose", "pwrite"})
    hle(name, return_minus_one);
  hle("sigaction", import_sigaction);
  hle("syscall", import_syscall);
  hle("dl_unwind_find_exidx", import_dl_unwind_find_exidx);
  hle("__gnu_Unwind_Find_exidx", import_gnu_Unwind_Find_exidx);
  hle("dlopen", import_dlopen);
  hle("dlsym", import_dlsym);
  hle("setlocale", import_setlocale);
  hle("getenv", import_getenv);
  hle("strerror", import_strerror);
  gen("setenv", "ppi>i");
  gen("unsetenv", "p>i");
  gen("usleep", "u>i");
  hle("sysconf", import_sysconf);
  gen("sched_yield", "v>i");
  gen("sched_get_priority_min", "i>i");
  gen("sched_get_priority_max", "i>i");
  gen("clock", "v>u");
  gen("fnmatch", "ppi>i");
}
