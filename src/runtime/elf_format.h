// On-disk ELF32 structures and the constants of the ARM ABI that the loader needs.
#pragma once

#include "common.h"

#pragma pack(push, 1)

struct Ehdr {
  uint8_t id[16];
  uint16_t type, machine;
  u32 version, entry, phoff, shoff, flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct Dyn {
  int32_t tag;
  u32 val;
};

struct Sym {
  u32 name, value, size;
  uint8_t info, other;
  uint16_t shndx;
};

struct Rel {
  u32 offset, info;  // info = symbol index << 8 | relocation type
};

#pragma pack(pop)

enum : uint16_t { EM_ARM = 40 };

enum : uint8_t { ELFCLASS32 = 1 };

// Program header types.
enum : u32 { PT_LOAD = 1, PT_DYNAMIC = 2, PT_ARM_EXIDX = 0x70000001 };

// Dynamic section tags.
enum : int32_t {
  DT_NULL = 0,
  DT_PLTRELSZ = 2,
  DT_HASH = 4,
  DT_STRTAB = 5,
  DT_SYMTAB = 6,
  DT_INIT = 12,
  DT_REL = 17,
  DT_RELSZ = 18,
  DT_JMPREL = 23,
  DT_INIT_ARRAY = 25,
  DT_INIT_ARRAYSZ = 27,
};

// ARM relocation types.
enum : u32 { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };
