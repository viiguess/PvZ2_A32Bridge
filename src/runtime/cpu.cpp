// The JIT lifecycle and the guest <-> host call path.
#include <stdlib.h>
#include <string.h>

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "dynarmic/interface/exclusive_monitor.h"

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
  jit = new Dynarmic::A32::Jit(conf);
}

Cpu::~Cpu() {
  delete jit;
  std::lock_guard<std::mutex> lock(g_cpu_mu);
  g_free_pid.push_back(pid);
}

u32 cpu_lr(Cpu* cpu) {
  return cpu->reg(14);
}

static Cpu* make_cpu(Thread* thr, int depth) {
  return new Cpu(thr, depth);
}

u32 guest_callv(u32 pc, const u32* args, int arg_count, u32* high_word) {
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
  cpu->jit->Run();
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

void Cpu::CallSVC(u32 idx) {
  if (idx == 0) {
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
    if (k < 300 || k % 200000 == 0)
      LOG("stub#%u %s lr=%08x", k, stub->name, jit->Regs()[14]);
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
