// The generic forwarder: most libc/libm imports are described by a signature string and forwarded to the host
// implementation found with dlsym. See libc_internal.h for the signature letters.
#include "libc/forward.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/stdio_imports.h"

typedef u64 (*FnI)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double,
                   double, double);
typedef float (*FnF)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double,
                     double, double);
typedef double (*FnD)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double,
                      double, double);

u32 to_guest(const void* host_ptr) {
  const uint8_t* bytes = (const uint8_t*)host_ptr;
  return (bytes >= G && bytes < G + (1ull << 32)) ? (u32)(bytes - G) : 0;
}

void set_errno() {
  *(int*)H(thread_errno_addr()) = errno;
}

void generic(Ctx& ctx, const Stub& stub) {
  u64 int_args[10] = {0};
  double float_args[8] = {0};
  int int_count = 0, float_count = 0, arg_index = 0;
  u32 guest_stream = 0;  // first FILE argument: its end-of-file and error state is mirrored into the guest struct
  const char* cursor = stub.sig;
  for (; *cursor && *cursor != '>'; cursor++) {
    switch (*cursor) {
      case 'p':
        int_args[int_count++] = (u64)(uintptr_t)H(ctx.a(arg_index++));
        break;
      case 's':
        int_args[int_count++] = (u64)(uintptr_t)guest_host_path((const char*)H(ctx.a(arg_index++)));
        break;
      case 'o': {
        u32 value = ctx.a(arg_index++);
        int_args[int_count++] = value < 0x10000 ? value : (u64)(uintptr_t)H(value);
      } break;
      case 'F':
        if (!guest_stream)
          guest_stream = ctx.a(arg_index);
        int_args[int_count++] = (u64)(uintptr_t)file_host(ctx.a(arg_index++));
        break;
      case 'i':
        int_args[int_count++] = (u64)(int64_t)(int32_t)ctx.a(arg_index++);
        break;
      case 'u':
        int_args[int_count++] = ctx.a(arg_index++);
        break;
      case 'q':
        if (arg_index & 1)
          arg_index++;
        int_args[int_count++] = ctx.q(arg_index);
        arg_index += 2;
        break;
      case 'f': {
        u32 bits = ctx.a(arg_index++);
        double number = 0;
        memcpy(&number, &bits, 4);
        float_args[float_count++] = number;
      } break;
      case 'd': {
        if (arg_index & 1)
          arg_index++;
        u64 bits = ctx.q(arg_index);
        arg_index += 2;
        double number;
        memcpy(&number, &bits, 8);
        float_args[float_count++] = number;
      } break;
    }
  }
  char return_kind = cursor[1];
#define A                                                                                                              \
  int_args[0], int_args[1], int_args[2], int_args[3], int_args[4], int_args[5], int_args[6], int_args[7], int_args[8], \
      int_args[9], float_args[0], float_args[1], float_args[2], float_args[3], float_args[4], float_args[5],           \
      float_args[6], float_args[7]
  if (return_kind == 'f') {
    float number = ((FnF)stub.fn)(A);
    u32 bits;
    memcpy(&bits, &number, 4);
    ctx.ret(bits);
    set_errno();
    return;
  }
  if (return_kind == 'd') {
    double number = ((FnD)stub.fn)(A);
    u64 bits;
    memcpy(&bits, &number, 8);
    ctx.ret64(bits);
    set_errno();
    return;
  }
  u64 result = ((FnI)stub.fn)(A);
#undef A
  set_errno();
  // Old bionic headers inline feof()/ferror() as tests of sFILE::_flags (offset 12: __SEOF 0x20, __SERR 0x40), so a
  // loop such as "while (!feof(f)) fgets(...)" only ends when the guest struct carries the host stream's state.
  if (guest_stream) {
    FILE* host = file_host(guest_stream);
    if (host) {
      uint16_t* flags = (uint16_t*)((uint8_t*)H(guest_stream) + 12);
      *flags = (uint16_t)((*flags & ~0x60) | (feof(host) ? 0x20 : 0) | (ferror(host) ? 0x40 : 0));
    }
  }
  switch (return_kind) {
    case 'q':
      ctx.ret64(result);
      break;
    case 'P':
      ctx.ret(to_guest((void*)(uintptr_t)result));
      break;
    case 'F':
      ctx.ret(file_new((FILE*)(uintptr_t)result));
      break;
    case 'v':
      break;
    default:
      ctx.ret((u32)result);
  }
}

void gen(const char* name, const char* sig, const char* lib) {
  void* fn = lib ? nullptr : dlsym(RTLD_DEFAULT, name);
  if (!fn && lib) {
    void* library = dlopen(lib, RTLD_NOW);
    if (library)
      fn = dlsym(library, name);
  }
  if (!fn) {
    LOG("host symbol missing: %s", name);
    return;
  }
  stub_add(name, generic, fn, sig);
}

void hle(const char* name, Handler library) {
  stub_add(name, library);
}
