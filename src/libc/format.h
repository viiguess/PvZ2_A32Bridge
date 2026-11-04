// printf-style formatting and scanf-style parsing of guest arguments.
#pragma once

#include <string>

#include "runtime/runtime.h"

// Cursor over printf-style varargs: either the stub's register/stack arguments or a guest va_list.
struct Args {
  Ctx* ctx;
  int next;     // next argument index (register/stack mode)
  u32 va_slot;  // next va_list slot, a guest address (va_list mode)
  bool is_va_list;

  u32 word();   // next 32-bit argument
  u64 dword();  // next 64-bit argument (aligned in register/stack mode)
};

std::string format_guest(const char* fmt, Args& args);

// sscanf/swscanf/fscanf with up to 8 output pointers; `source_index` is the argument holding the string/FILE.
void scan_guest(Ctx& ctx, int source_index, bool wide, bool file);
