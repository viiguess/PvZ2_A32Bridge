// printf-style formatting and scanf-style parsing of guest arguments.
#include "libc/format.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"
#include "libc/stdio_imports.h"

// ------------------------------------------------------------------ printf / scanf support
u32 Args::word() {
  if (is_va_list) {
    u32 value = *(u32*)H(va_slot);
    va_slot += 4;
    return value;
  }
  return ctx->a(next++);
}

u64 Args::dword() {
  if (is_va_list) {
    va_slot = (va_slot + 7) & ~7u;
    u64 value = *(u64*)H(va_slot);
    va_slot += 8;
    return value;
  }
  if (next & 1)
    next++;
  u64 value = ctx->q(next);
  next += 2;
  return value;
}

std::string format_guest(const char* fmt, Args& a) {
  std::string out;
  char buf[128];
  for (const char* p = fmt; *p;) {
    if (*p != '%') {
      out += *p++;
      continue;
    }
    if (p[1] == '%') {
      out += '%';
      p += 2;
      continue;
    }
    std::string spec = "%";
    p++;
    while (strchr("-+ #0'", *p))
      spec += *p++;
    auto num = [&]() {
      if (*p == '*') {
        int value = (int)a.word();
        p++;
        spec += std::to_string(value);
      } else
        while (*p >= '0' && *p <= '9')
          spec += *p++;
    };
    num();
    if (*p == '.') {
      spec += *p++;
      num();
    }
    bool is_long_long = false, is_wide = false;
    int short_count = 0;
    for (;; p++) {
      if (*p == 'l' && p[1] == 'l') {
        is_long_long = true;
        p++;
      } else if (*p == 'j')
        is_long_long = true;
      else if (*p == 'h')
        short_count++;
      else if (*p == 'l')
        is_wide = true;
      else if (*p == 'z' || *p == 't' || *p == 'L') {
      } else
        break;
    }
    char conv = *p++;
    if (!conv)
      break;
    std::string tail;
    if (short_count)
      spec += std::string(short_count, 'h');
    switch (conv) {
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'c': {
        if (is_long_long) {
          spec += "ll";
          spec += conv;
          snprintf(buf, sizeof buf, spec.c_str(), (long long)a.dword());
        } else {
          spec += conv;
          snprintf(buf, sizeof buf, spec.c_str(), (int)a.word());
        }
        out += buf;
        break;
      }
      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        u64 bits = a.dword();
        double number;
        memcpy(&number, &bits, 8);
        spec += conv;
        snprintf(buf, sizeof buf, spec.c_str(), number);
        out += buf;
        break;
      }
      case 's': {
        u32 guest_str = a.word();
        if (is_wide && guest_str) {  // %ls: guest wchar_t string -> UTF-8 (BMP only)
          std::string text;
          for (const u32* wide_char = (const u32*)H(guest_str); *wide_char; wide_char++) {
            u32 code_point = *wide_char;
            if (code_point < 0x80)
              text += (char)code_point;
            else if (code_point < 0x800) {
              text += (char)(0xC0 | code_point >> 6);
              text += (char)(0x80 | (code_point & 0x3F));
            } else {
              text += (char)(0xE0 | code_point >> 12);
              text += (char)(0x80 | (code_point >> 6 & 0x3F));
              text += (char)(0x80 | (code_point & 0x3F));
            }
          }
          out += text;
          break;
        }
        const char* host_str = guest_str ? (const char*)H(guest_str) : "(null)";
        spec += 's';
        int length = snprintf(nullptr, 0, spec.c_str(), host_str);
        std::string text(length + 1, 0);
        snprintf(&text[0], length + 1, spec.c_str(), host_str);
        text.resize(length);
        out += text;
        break;
      }
      case 'p':
        snprintf(buf, sizeof buf, "0x%x", a.word());
        out += buf;
        break;
      case 'n':
        a.word();
        break;
      default:
        out += '%';
        out += conv;
    }
  }
  return out;
}

void scan_guest(Ctx& ctx, int src_idx, bool wide, bool file) {
  void* src = file ? (void*)file_host(ctx.a(src_idx)) : ctx.p(src_idx);
  void* fmt = ctx.p(src_idx + 1);
  void* value[8];
  for (int i = 0; i < 8; i++)
    value[i] = H(ctx.a(src_idx + 2 + i));
  int result;
  if (file)
    result = fscanf((FILE*)src, (const char*)fmt, value[0], value[1], value[2], value[3], value[4], value[5], value[6],
                    value[7]);
  else if (wide)
    result = swscanf((const wchar_t*)src, (const wchar_t*)fmt, value[0], value[1], value[2], value[3], value[4],
                     value[5], value[6], value[7]);
  else
    result = sscanf((const char*)src, (const char*)fmt, value[0], value[1], value[2], value[3], value[4], value[5],
                    value[6], value[7]);
  ctx.ret(result);
}
