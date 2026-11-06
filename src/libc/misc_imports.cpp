// Process, environment, dynamic-loader and math-classification imports.
#include "libc/misc_imports.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"

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

static void abort_process(Ctx& ctx, const Stub&) {
  LOG("guest abort()");
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

static void import_dlopen(Ctx& ctx, const Stub&) {
  ctx.ret(1);
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
