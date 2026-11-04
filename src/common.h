// Shared types, logging, and the guest address-space layout.
//
// The "guest" is the 32-bit ARM program; the "host" is this arm64 library. The whole 4 GiB guest address space is
// one host reservation (`G`), so a guest address is converted to a host pointer by adding `G`.
#pragma once

#include <android/log.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

// Logging is compiled out unless built with -DA32_LOG (translator messages and the game's own log/stdio output).
#ifdef A32_LOG
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "a32", __VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

extern uint8_t* G;  // host address of guest address 0

static inline void* H(u32 guest) {
  return guest ? (void*)(G + guest) : nullptr;
}

static inline u32 GA(const void* host) {
  return host ? (u32)((const uint8_t*)host - G) : 0;
}

// Guest address-space layout.
enum : u32 {
  STUB_BASE = 0x0F000000,  // svc stubs, 8 bytes each: `svc #n; bx lr`
  STUB_SIZE = 0x00400000,
  DATA_BASE = 0x0F400000,  // permanent guest data: ctype tables, __sF, interface ids, vtables
  DATA_SIZE = 0x00400000,
  RET_ADDR = STUB_BASE,   // sentinel return address: a guest function returning here hands control back to the host
  IMG_BASE = 0x10000000,  // the guest library is loaded here
  HEAP_BASE = 0x30000000,
  HEAP_END = 0x80000000,
  KUSER_BASE = 0xFFFF0000,  // Linux "kuser helper" page (see symbols.cpp)
};
