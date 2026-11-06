// The JIT lifecycle and the guest <-> host call path.
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "dynarmic/interface/exclusive_monitor.h"

#include "runtime/code_dump.h"
#include "runtime/jit.h"
#include "runtime/runtime.h"
#include "runtime/thread.h"

// Every mapped guest page points at G (absolute_offset_page_table), so loads/stores are inlined by the JIT;
// unmapped pages fall back to the Memory* callbacks which report the fault.
using PageTable = std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
static PageTable* g_pt;
static Dynarmic::ExclusiveMonitor* g_mon;  // shared by all JITs: ldrex/strex are real host atomics
static std::mutex g_cpu_mu;
static std::vector<size_t> g_free_pid;
static std::vector<Cpu*> g_all_cpus;
static size_t g_next_pid;

enum { MAX_JITS = 512 };

Cpu::Cpu(Thread* thr, int depth) : thr(thr) {
  static std::once_flag once;
  std::call_once(once, [] {
    g_pt = new PageTable();
    for (u32 pg = 0; pg < (1u << 20); pg++)
      if (mapped(pg << 12))
        (*g_pt)[pg] = G;
    g_mon = new Dynarmic::ExclusiveMonitor(MAX_JITS);
    ((u32*)(G + STUB_BASE))[0] = 0xEF000000;  // svc #0: guest "returned" to RET_ADDR -> host
    ((u32*)(G + STUB_BASE))[1] = 0xE12FFF1E;
  });
  {
    std::lock_guard<std::mutex> lock(g_cpu_mu);
    if (!g_free_pid.empty()) {
      pid = g_free_pid.back();
      g_free_pid.pop_back();
    } else if (g_next_pid < MAX_JITS)
      pid = g_next_pid++;
    else {
      LOG("too many JIT instances");
      abort();
    }
  }
  cp15 = std::make_shared<Cp15>();
  cp15->tpidruro = thr->tls;
  Dynarmic::A32::UserConfig conf;
  conf.callbacks = this;
  conf.processor_id = pid;
  conf.global_monitor = g_mon;
  conf.arch_version = Dynarmic::A32::ArchVersion::v7;
  conf.page_table = g_pt;
  conf.absolute_offset_page_table = true;
  conf.coprocessors[15] = cp15;
  conf.enable_cycle_counting = false;  // run until the guest returns to RET_ADDR
  conf.always_little_endian = true;
  conf.define_unpredictable_behaviour = true;
  conf.wall_clock_cntpct = true;
  conf.code_cache_size = (depth == 0 ? 96u : 16u) << 20;  // virtual only; the JIT flushes when full
  // Games need no bit-exact NaN/FMA behaviour; the global monitor stays exact for ldrex/strex locks.
  conf.unsafe_optimizations = true;
  conf.optimizations = Dynarmic::all_safe_optimizations | Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA |
                       Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                       Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                       Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
  jit = new Dynarmic::A32::Jit(conf);
  std::lock_guard<std::mutex> lock(g_cpu_mu);
  g_all_cpus.push_back(this);
}

void cpu_invalidate_range(u32 start, u32 size) {
  code_dump_on_flush(start, size);
  std::lock_guard<std::mutex> lock(g_cpu_mu);
  for (Cpu* cpu : g_all_cpus)
    cpu->jit->InvalidateCacheRange(start, size);  // halts a running JIT; guest_callv resumes it
}

Cpu::~Cpu() {
  delete jit;
  std::lock_guard<std::mutex> lock(g_cpu_mu);
  g_all_cpus.erase(std::find(g_all_cpus.begin(), g_all_cpus.end(), this));
  g_free_pid.push_back(pid);
}

u32 cpu_lr(Cpu* cpu) {
  return cpu->reg(14);
}

static Cpu* make_cpu(Thread* thr, int depth) {
  return new Cpu(thr, depth);
}

u32 guest_callv(u32 pc, const u32* args, int arg_count, u32* high_word) {
  if ((pc & ~1u) == 0) {  // a null guest function pointer: nothing to run
    LOG("guest call to address 0 ignored (host caller %p)", __builtin_return_address(0));
    if (high_word)
      *high_word = 0;
    return 0;
  }
  Thread* thr = thread();
  int depth = ++thr->depth;
  if ((int)thr->cpus.size() <= depth)
    thr->cpus.push_back(make_cpu(thr, depth));
  Cpu* cpu = thr->cpus[depth];
  Cpu* saved_running = thr->running;
  u32 saved_hint = thr->sp_hint;
  u32 sp = (depth == 0 ? thr->stack_top : thr->sp_hint - 512) & ~7u;
  u32 arg_words = (u32)arg_count;
  if (arg_words > 4) {
    sp = (sp - 4 * (arg_words - 4)) & ~7u;
    memcpy(G + sp, args + 4, 4 * (arg_words - 4));
  }
  auto& regs = cpu->jit->Regs();
  for (u32 i = 0; i < 4; i++)
    regs[i] = i < arg_words ? args[i] : 0;
  regs[13] = sp;
  regs[14] = RET_ADDR;
  regs[15] = pc & ~1u;
  cpu->jit->SetCpsr(0x10 | ((pc & 1) ? 0x20 : 0));  // user mode, Thumb if the entry address says so
  cpu->jit->ClearExclusiveState();
  cpu->jit->ClearHalt();
  cpu->fault = false;
  thr->running = cpu;
  thr->sp_hint = sp;
  Dynarmic::HaltReason reason;
  do  // a cache invalidation halts the JIT mid-call: resume
    reason = cpu->jit->Run();
  while (Dynarmic::Has(reason, Dynarmic::HaltReason::CacheInvalidation) &&
         !Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined1) && !cpu->fault);
  if (cpu->fault && !thr->exit_req)
    LOG("guest fault at pc=%08x entry=%08x", regs[15], pc);
  u32 out0 = regs[0], out1 = regs[1];
  thr->running = saved_running;
  thr->sp_hint = saved_hint;
  thr->depth--;
  if (high_word)
    *high_word = out1;
  return out0;
}

u32 guest_call(u32 pc, std::initializer_list<u32> a, u32* high_word) {
  return guest_callv(pc, a.begin(), (int)a.size(), high_word);
}

void (*g_syscall_hook)(uint32_t* regs) = nullptr;

#ifdef A32_TRACE
#ifndef A32_TRACE_LIMIT
#define A32_TRACE_LIMIT 300  // number of calls traced from the start (then one in 200000)
#endif

// For stubs that take a path or a name, the string is printed as well.
static const char* traced_string(const char* stub, const std::array<uint32_t, 16>& regs) {
  static const char* const first_arg[] = {"open", "fopen", "access", "stat", "opendir", "mkdir", "remove", "dlopen", "getenv"};
  static const char* const second_arg[] = {"strstr", "dlsym"};  // (haystack/handle, needle/name)
  for (const char* name : first_arg)
    if (!strcmp(stub, name) && regs[0] > 0x1000 && regs[0] < KUSER_BASE)
      return (const char*)H(regs[0]);
  for (const char* name : second_arg)
    if (!strcmp(stub, name) && regs[1] > 0x1000 && regs[1] < KUSER_BASE)
      return (const char*)H(regs[1]);
  return "";
}
#endif

void Cpu::CallSVC(u32 idx) {
  if (idx == 0) {
    if (g_syscall_hook && jit->Regs()[15] != RET_ADDR + 4) {
      g_syscall_hook(jit->Regs().data());
      return;
    }
    jit->HaltExecution();
    return;
  }  // return to RET_ADDR
  const Stub* stub = stub_by_index(idx);
  if (!stub) {
    LOG("bad stub %u", idx);
    fault = true;
    jit->HaltExecution();
    return;
  }
#ifdef A32_TRACE
  {
    static std::atomic<unsigned> n;
    unsigned k = n++;
    if (k < A32_TRACE_LIMIT || k % 200000 == 0) {
      auto& r = jit->Regs();
      LOG("stub#%u %s lr=%08x r0=%08x r1=%08x r2=%08x %s", k, stub->name, r[14], r[0], r[1], r[2],
          traced_string(stub->name, r));
    }
  }
#endif
  Ctx ctx;
  ctx.cpu = this;
  auto& regs = jit->Regs();
  for (int i = 0; i < 4; i++)
    ctx.r[i] = regs[i];
  ctx.sp = regs[13];
  thr->sp_hint = ctx.sp;
  stub->handler(ctx, *stub);
  regs[0] = ctx.o0;
  if (ctx.wide)
    regs[1] = ctx.o1;
}
