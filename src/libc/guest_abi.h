// Layouts and constants of the 32-bit ARM bionic ABI that differ from the arm64 host's.
//
// The guest library was compiled against 32-bit bionic, so every structure it shares with libc has the 32-bit
// layout. The imports convert between these and the host structures.
#pragma once

#include "common.h"

// ---- stdio ----------------------------------------------------------------------------------------------------
// A guest FILE is an opaque 84-byte block (the size of bionic's 32-bit FILE) whose first word we use to hold the
// host FILE*. stdin/stdout/stderr are the three entries of the guest's `__sF` array.
enum : u32 {
  GUEST_FILE_SIZE = 84,
  GUEST_STD_STREAMS = 3,
};

enum StdStream { STD_IN = 0, STD_OUT = 1, STD_ERR = 2, NOT_STD = -1 };

// ---- struct stat ----------------------------------------------------------------------------------------------
// Byte offsets inside the 104-byte 32-bit bionic `struct stat`.
enum GuestStatOffset : int {
  STAT_DEV = 0,  // u64
  STAT_INO32 = 12,
  STAT_MODE = 16,
  STAT_NLINK = 20,
  STAT_UID = 24,
  STAT_GID = 28,
  STAT_RDEV = 32,  // u64
  STAT_SIZE = 48,  // u64
  STAT_BLKSIZE = 56,
  STAT_BLOCKS = 64,  // u64
  STAT_ATIME = 72,   // sec, nsec
  STAT_MTIME = 80,
  STAT_CTIME = 88,
  STAT_INO64 = 96,  // u64
  STAT_STRUCT_SIZE = 104,
};

// ---- open(2) -------------------------------------------------------------------------------------------------
// These four flag values differ between arm32 and arm64 Linux.
enum : u32 {
  ARM32_O_DIRECTORY = 040000,
  ARM32_O_NOFOLLOW = 0100000,
  ARM32_O_DIRECT = 0200000,
  ARM32_O_LARGEFILE = 0400000,
};

// ---- pthread_attr_t -------------------------------------------------------------------------------------------
// 24 bytes in 32-bit bionic.
struct GuestPthreadAttr {
  u32 flags;  // bit 0: detached
  u32 stack_base;
  u32 stack_size;
  u32 guard_size;
  u32 sched_policy;
  u32 sched_priority;
};

enum : u32 {
  ATTR_DETACHED = 1,
  DEFAULT_GUEST_STACK = 1u << 20,
  DEFAULT_GUARD_SIZE = 4096,
  MIN_GUEST_STACK = 64 * 1024,
};
