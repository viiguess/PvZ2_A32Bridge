// Registers the libc/libm/zlib/GLES2 imports and the guest-visible data they rely on.
//
// Most functions are described by a signature string and forwarded to the host (libc_forward.cpp); anything whose
// layout differs between 32-bit and 64-bit bionic is written out by hand in the libc_*.cpp modules.
#include "libc/libc.h"

#include <ctype.h>

#include <utility>

#include "libc/forward.h"
#include "libc/gles_imports.h"
#include "libc/memory_imports.h"
#include "libc/misc_imports.h"
#include "libc/pthread_imports.h"
#include "libc/stdio_imports.h"
#include "libc/time_imports.h"
#include "libc/zlib_imports.h"

// ctype tables in the layout of old bionic headers: _ctype_[c + 1] bit flags, plus tolower/toupper tables.
// The three symbols the guest imports are *pointer variables* to the tables, not the tables themselves.
static void init_ctype_tables() {
  u32 ctype = gdata_alloc(257, 8);
  u32 lower = gdata_alloc(257 * 2, 8), upper = gdata_alloc(257 * 2, 8);
  uint8_t* flags = (uint8_t*)H(ctype);
  int16_t* tolower_tab = (int16_t*)H(lower);
  int16_t* toupper_tab = (int16_t*)H(upper);
  flags[0] = 0;
  tolower_tab[0] = toupper_tab[0] = -1;  // EOF
  for (int i = 0; i < 256; i++) {
    uint8_t class_bits = 0;
    if (i < 128) {
      if (isupper(i))
        class_bits |= 0x01;
      if (islower(i))
        class_bits |= 0x02;
      if (isdigit(i))
        class_bits |= 0x04;
      if (isspace(i))
        class_bits |= 0x08;
      if (ispunct(i))
        class_bits |= 0x10;
      if (iscntrl(i))
        class_bits |= 0x20;
      if (isxdigit(i))
        class_bits |= 0x40;
      if (i == ' ')
        class_bits |= 0x80;
    }
    flags[i + 1] = class_bits;
    tolower_tab[i + 1] = i < 128 ? tolower(i) : i;
    toupper_tab[i + 1] = i < 128 ? toupper(i) : i;
  }
  for (auto entry : {std::make_pair("_ctype_", ctype), std::make_pair("_tolower_tab_", lower),
                     std::make_pair("_toupper_tab_", upper)}) {
    u32 cell = gdata_alloc(4, 4);
    *(u32*)H(cell) = entry.second;
    sym_add(entry.first, cell);
  }
}

void libc_init() {
  init_ctype_tables();

  u32 stack_guard = gdata_alloc(4, 4);
  *(u32*)H(stack_guard) = 0x5a17c0de;
  sym_add("__stack_chk_guard", stack_guard);

  // setjmp / longjmp as guest code. jmp_buf layout: r4-r11, sp, lr, d8-d15.
  static const u32 setjmp_code[] = {0xE8A06FF0, 0xECA08B10, 0xE3A00000, 0xE12FFF1E};
  static const u32 longjmp_code[] = {0xE8B06FF0, 0xECB08B10, 0xE3510000, 0x03A01001, 0xE1A00001, 0xE12FFF1E};
  sym_add_asm("setjmp", setjmp_code, 4);
  sym_add_asm("longjmp", longjmp_code, 6);

  register_memory_imports();
  register_misc_imports();
  register_time_imports();
  register_stdio_imports();
  register_pthread_imports();
  register_zlib_imports();
  register_gles_imports();
}
