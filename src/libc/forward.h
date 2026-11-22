// The generic forwarder: most libc/libm imports are described by a signature string and forwarded to the host
// implementation found with dlsym.
//
// A signature is "<args>><return>", one letter per argument:
//   p  guest pointer -> host pointer            s  guest path string -> host path (see guest_host_path)
//   o  pointer-or-GL-offset (values below GUEST_MIN_POINTER pass through)
//   F  guest FILE* -> host FILE*                i  s32      u  u32      q  64-bit (aligned register pair)
//   f  float                                    d  double
// Return letters: v void, i/u 32-bit, q 64-bit, f, d, P host pointer -> guest pointer, F host FILE* -> guest FILE*.
#pragma once

#include "runtime/runtime.h"

// Guest values below this are treated as small integers rather than pointers where an argument can be either
// (ioctl's third argument, glVertexAttribPointer offsets). No guest allocation lives that low.
enum : u32 { GUEST_MIN_POINTER = 0x10000 };

void generic(Ctx& ctx, const Stub& stub);                                // handler for signature-described imports
void gen(const char* name, const char* sig, const char* lib = nullptr);  // forward `name` (from `lib`, default libc)
void hle(const char* name, Handler handler);                             // register a hand-written handler
void set_errno();                                                        // publish the host errno to the guest
u32 to_guest(const void* host_ptr);  // host pointer inside the reservation -> guest address
