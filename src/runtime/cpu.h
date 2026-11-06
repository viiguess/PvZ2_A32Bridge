// Guest execution. Each host thread that touches the guest lazily gets a `Thread`; nested host -> guest calls
// (for example a guest callback invoked from inside a stub) run on a deeper `Cpu` of the same thread.
#pragma once

#include <initializer_list>

#include "common.h"

struct Cpu;
struct Thread;

Thread* thread();
Thread* thread_create_for_guest(u32 stack_size, u32 id);  // for threads started by the guest's pthread_create
u32 thread_alloc_id();
u32 thread_self_id();
u32 thread_errno_addr();  // guest address of this thread's errno
u32 thread_tls_addr();
u32 thread_stack_lo();
u32 thread_stack_size();
void thread_set_sp_hint(u32 sp);
void thread_request_exit();

u32 cpu_lr(Cpu* cpu);  // guest LR of a running CPU (diagnostics)

// Calls a guest function (Thumb if bit 0 of `pc` is set) and returns r0; r1 receives the high word of 64-bit results.
u32 guest_callv(u32 pc, const u32* args, int count, u32* high_word);
u32 guest_call(u32 pc, std::initializer_list<u32> args, u32* high_word = nullptr);

// Drops translated code for [start, start + size) on every thread, after guest code was patched.
void cpu_invalidate_range(u32 start, u32 size);

// Optional: emulation of Linux system calls. Guest code that issues a raw `svc #0` (EABI: number in r7, arguments in
// r0-r5) is passed to this hook with the 16 guest registers; it stores the result in regs[0]. Without a hook such an
// svc is treated as "return to the host".
extern void (*g_syscall_hook)(uint32_t* regs);
