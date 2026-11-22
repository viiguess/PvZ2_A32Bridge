// The Dynarmic glue: guest memory callbacks, the CP15 coprocessor, and one `Cpu` (= one JIT instance).
#pragma once

#include <memory>
#include <optional>
#include <variant>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "runtime/runtime.h"

struct Thread;

// Guest addresses that are backed by memory.
inline bool mapped(u32 a) {
  return a >= KUSER_BASE || (a >= STUB_BASE && a < STUB_BASE + STUB_SIZE + DATA_SIZE) ||
         (a >= IMG_BASE && a < HEAP_END);
}

// CP15: TPIDRURO (thread pointer) + cache/barrier maintenance no-ops
struct Cp15 final : Dynarmic::A32::Coprocessor {
  using Reg = Dynarmic::A32::CoprocReg;
  u32 tpidruro = 0, tpidrurw = 0;

  static u64 nop(void*, u32, u32) {
    return 0;
  }

  std::optional<Callback> CompileInternalOperation(bool, unsigned, Reg, Reg, Reg, unsigned) override {
    return std::nullopt;
  }

  CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc1, Reg crn, Reg crm, unsigned opc2) override {
    if (opc1 == 0 && crn == Reg::C13 && crm == Reg::C0 && opc2 == 2)
      return &tpidrurw;
    if (opc1 == 0 && crn == Reg::C7)
      return Callback{nop, std::nullopt};
    return std::monostate{};
  }

  CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, Reg) override {
    return std::monostate{};
  }

  CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc1, Reg crn, Reg crm, unsigned opc2) override {
    if (opc1 == 0 && crn == Reg::C13 && crm == Reg::C0) {
      if (opc2 == 3)
        return &tpidruro;
      if (opc2 == 2)
        return &tpidrurw;
    }
    return std::monostate{};
  }

  CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, Reg) override {
    return std::monostate{};
  }

  std::optional<Callback> CompileLoadWords(bool, bool, Reg, std::optional<uint8_t>) override {
    return std::nullopt;
  }

  std::optional<Callback> CompileStoreWords(bool, bool, Reg, std::optional<uint8_t>) override {
    return std::nullopt;
  }
};

struct Cpu final : Dynarmic::A32::UserCallbacks {
  Dynarmic::A32::Jit* jit = nullptr;
  Thread* thr = nullptr;
  std::shared_ptr<Cp15> cp15;
  size_t pid = 0;
  bool fault = false;
  Cpu(Thread* owner, int depth);
  ~Cpu() override;

  u32& reg(int i) {
    return jit->Regs()[i];
  }

  void bad(u32 a, int write, int size) {
    LOG("BAD MEM %s addr=%x size=%d pc=%08x lr=%08x sp=%08x", write ? "write" : "read", a, size, reg(15), reg(14),
        reg(13));
    LOG("  r0-r3 %08x %08x %08x %08x r4-r7 %08x %08x %08x %08x r8-r12 %08x %08x %08x %08x %08x", reg(0), reg(1), reg(2),
        reg(3), reg(4), reg(5), reg(6), reg(7), reg(8), reg(9), reg(10), reg(11), reg(12));
    u32 code = reg(15) & ~1u;
    if (mapped(code) && mapped(code + 31)) {
      char hex[80] = {};
      for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", G[code + i]);
      LOG("  code at pc: %s", hex);
    }
    fault = true;
    jit->HaltExecution();
  }

  template <class T>
  T rd(u32 a) {
    if (!mapped(a) || !mapped(a + sizeof(T) - 1)) {
      bad(a, 0, sizeof(T));
      return 0;
    }
    T v;
    memcpy(&v, G + a, sizeof v);
    return v;
  }

  template <class T>
  void wr(u32 a, T v) {
    if (!mapped(a) || !mapped(a + sizeof(T) - 1)) {
      bad(a, 1, sizeof(T));
      return;
    }
    memcpy(G + a, &v, sizeof v);
  }

  template <class T>
  bool wrx(u32 a, T v, T expected) {
    if (!mapped(a)) {
      bad(a, 1, sizeof(T));
      return false;
    }
    return __atomic_compare_exchange_n((T*)(G + a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }

  uint8_t MemoryRead8(u32 a) override {
    return rd<uint8_t>(a);
  }

  uint16_t MemoryRead16(u32 a) override {
    return rd<uint16_t>(a);
  }

  uint32_t MemoryRead32(u32 a) override {
    return rd<uint32_t>(a);
  }

  uint64_t MemoryRead64(u32 a) override {
    return rd<uint64_t>(a);
  }

  void MemoryWrite8(u32 a, uint8_t v) override {
    wr(a, v);
  }

  void MemoryWrite16(u32 a, uint16_t v) override {
    wr(a, v);
  }

  void MemoryWrite32(u32 a, uint32_t v) override {
    wr(a, v);
  }

  void MemoryWrite64(u32 a, uint64_t v) override {
    wr(a, v);
  }

  bool MemoryWriteExclusive8(u32 a, uint8_t v, uint8_t e) override {
    return wrx(a, v, e);
  }

  bool MemoryWriteExclusive16(u32 a, uint16_t v, uint16_t e) override {
    return wrx(a, v, e);
  }

  bool MemoryWriteExclusive32(u32 a, uint32_t v, uint32_t e) override {
    return wrx(a, v, e);
  }

  bool MemoryWriteExclusive64(u32 a, uint64_t v, uint64_t e) override {
    return wrx(a, v, e);
  }

  void InterpreterFallback(u32 pc, size_t n) override {
    LOG("JIT cannot translate %zu instruction(s) at pc=%08x (word %08x)", n, pc, mapped(pc) ? *(u32*)(G + pc) : 0);
    fault = true;
    jit->HaltExecution();
  }

  void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override {
    LOG("guest exception %d at pc=%08x (word %08x)", (int)e, pc, mapped(pc) ? *(u32*)(G + pc) : 0);
    fprintf(stderr, "[a32]   lr %08x sp %08x r0-r3 %08x %08x %08x %08x r4-r7 %08x %08x %08x %08x\n", reg(14), reg(13), reg(0), reg(1), reg(2),
        reg(3), reg(4), reg(5), reg(6), reg(7));
    fault = true;
    jit->HaltExecution();
  }

  void AddTicks(uint64_t) override {
  }

  uint64_t GetTicksRemaining() override {
    return ~0ull;
  }

  void CallSVC(u32 swi) override;
};
