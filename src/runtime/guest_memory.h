// Guest memory: the 4 GiB reservation (`G`, see common.h), a permanent data arena and the guest heap.
#pragma once

#include "common.h"

u32 gmalloc(u32 n);
void gfree(u32 ptr);
u32 gmemalign(u32 alignment, u32 n);
u32 gdata_alloc(u32 n, u32 alignment = 8);  // permanent allocation, never freed
u32 gstr(const char* text);                 // interned guest copy of a host string
