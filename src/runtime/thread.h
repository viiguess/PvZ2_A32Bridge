// Per-host-thread state of the guest: its stack, TLS block, and the stack of JITs used for nested calls.
#pragma once

#include <vector>

#include "common.h"

struct Cpu;

struct Thread {
  std::vector<Cpu*> cpus;  // one JIT per nesting depth of host -> guest calls
  int depth = -1;          // current nesting depth (-1: not running guest code)
  u32 stack_lo = 0, stack_top = 0;
  u32 sp_hint = 0;  // sp to continue from when a stub calls back into the guest
  u32 tls = 0;      // guest TLS block; errno lives at its start
  u32 id = 0;
  bool exit_req = false;
  Cpu* running = nullptr;
};
