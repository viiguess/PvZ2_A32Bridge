// Memory allocation, number parsing and sorting.
#include "libc/memory_imports.h"

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

static void import_malloc(Ctx& ctx, const Stub&) {
  ctx.ret(gmalloc(ctx.a(0)));
}

static void import_free(Ctx& ctx, const Stub&) {
  gfree(ctx.a(0));
}

static void import_calloc(Ctx& ctx, const Stub&) {
  u64 size = (u64)ctx.a(0) * ctx.a(1);
  u32 ptr = size > 0x40000000 ? 0 : gmalloc((u32)size);
  if (ptr)
    memset(H(ptr), 0, (u32)size);
  ctx.ret(ptr);
}

static void import_realloc(Ctx& ctx, const Stub&) {
  u32 ptr = ctx.a(0), size = ctx.a(1);
  if (!ptr) {
    ctx.ret(gmalloc(size));
    return;
  }
  u32 cap = ((u32*)H(ptr))[-1] == ptr ? ((u32*)H(ptr))[-2] : 0;
  if (cap >= size && cap) {
    ctx.ret(ptr);
    return;
  }
  u32 new_ptr = gmalloc(size);
  if (new_ptr) {
    u32 old = cap ? cap : size;  // memalign'd blocks: copy at most n
    memcpy(H(new_ptr), H(ptr), std::min(old, size));
    gfree(ptr);
  }
  ctx.ret(new_ptr);
}

static void import_memalign(Ctx& ctx, const Stub&) {
  ctx.ret(gmemalign(ctx.a(0), ctx.a(1)));
}

static void import_aeabi_memset(Ctx& ctx, const Stub&) {
  memset(ctx.p(0), (int)ctx.a(2), ctx.a(1));
}

static void strtol_family(Ctx& ctx, const Stub& stub) {
  char* end;
  const char* str = (const char*)ctx.p(0);
  int base = ctx.a(2);
  const char* size = stub.name;
  bool sig = !strcmp(size, "strtol") || !strcmp(size, "strtoll");
  bool is_long_long = !strcmp(size, "strtoll") || !strcmp(size, "strtoull");
  long long value = sig ? strtoll(str, &end, base) : (long long)strtoull(str, &end, base);
  if (ctx.a(1))
    *(u32*)ctx.p(1) = GA(end);
  set_errno();
  if (is_long_long)
    ctx.ret64(value);
  else
    ctx.ret((u32)value);
}

static void import_strtod(Ctx& ctx, const Stub&) {
  char* end;
  double number = strtod((const char*)ctx.p(0), &end);
  if (ctx.a(1))
    *(u32*)ctx.p(1) = ctx.a(0) + (u32)(end - (const char*)ctx.p(0));
  u64 bits;
  memcpy(&bits, &number, 8);
  ctx.ret64(bits);
}

static void import_wcstol(Ctx& ctx, const Stub&) {
  wchar_t* end;
  const wchar_t* text = (const wchar_t*)ctx.p(0);
  long value = wcstol(text, &end, (int)ctx.a(2));
  if (ctx.a(1))
    *(u32*)ctx.p(1) = ctx.a(0) + 4 * (u32)(end - text);
  ctx.ret((u32)value);
}

// qsort(base, count, element_size, compare): sorts an index permutation with the guest comparator, then rearranges
// the elements. The comparator runs guest code, so std::sort's requirements on it are met by using a stable sort.
static void import_qsort(Ctx& ctx, const Stub&) {
  u32 base = ctx.a(0), count = ctx.a(1), element_size = ctx.a(2), compare = ctx.a(3);
  if (count < 2)
    return;
  std::vector<u32> order(count);
  for (u32 i = 0; i < count; i++)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](u32 lhs, u32 rhs) {
    return (int32_t)guest_call(compare, {base + lhs * element_size, base + rhs * element_size}) < 0;
  });
  std::vector<uint8_t> sorted((size_t)count * element_size);
  for (u32 i = 0; i < count; i++)
    memcpy(&sorted[(size_t)i * element_size], G + base + order[i] * element_size, element_size);
  memcpy(G + base, sorted.data(), sorted.size());
}

static void import_aeabi_memclr(Ctx& ctx, const Stub&) {
  memset(ctx.p(0), 0, ctx.a(1));
}

void register_memory_imports() {
  hle("malloc", import_malloc);
  hle("free", import_free);
  hle("calloc", import_calloc);
  hle("realloc", import_realloc);
  hle("memalign", import_memalign);
  gen("memcpy", "ppu>P");
  gen("memmove", "ppu>P");
  gen("memset", "pii>P");
  gen("memcmp", "ppu>i");
  gen("memchr", "piu>P");
  stub_add("__aeabi_memcpy", generic, dlsym(RTLD_DEFAULT, "memcpy"), "ppu>v");
  stub_add("__aeabi_memmove", generic, dlsym(RTLD_DEFAULT, "memmove"), "ppu>v");
  hle("__aeabi_memset", import_aeabi_memset);
  for (const char* name : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"})
    hle(name, import_aeabi_memclr);
  for (const char* name : {"__aeabi_memcpy4", "__aeabi_memcpy8"})
    stub_add(name, generic, dlsym(RTLD_DEFAULT, "memcpy"), "ppu>v");
  for (const char* name : {"__aeabi_memmove4", "__aeabi_memmove8"})
    stub_add(name, generic, dlsym(RTLD_DEFAULT, "memmove"), "ppu>v");

  // strings and ctype
  gen("strlen", "p>u");
  gen("strcmp", "pp>i");
  gen("strncmp", "ppu>i");
  gen("strcasecmp", "pp>i");
  gen("strncasecmp", "ppu>i");
  gen("strcpy", "pp>P");
  gen("strncpy", "ppu>P");
  gen("strcat", "pp>P");
  gen("strncat", "ppu>P");
  gen("strchr", "pi>P");
  gen("strstr", "pp>P");
  gen("strcspn", "pp>u");
  gen("atoll", "p>q");
  gen("strpbrk", "pp>P");
  gen("strtok", "pp>P");
  gen("strlcpy", "ppu>u");
  gen("strcoll", "pp>i");
  gen("strxfrm", "ppu>u");
  gen("atoi", "p>i");
  gen("atol", "p>i");
  gen("mktemp", "p>P");
  gen("isspace", "i>i");
  gen("isupper", "i>i");
  gen("isblank", "i>i");
  gen("tolower", "i>i");
  gen("toupper", "i>i");
  for (const char* size : {"strtol", "strtoul", "strtoll", "strtoull"})
    hle(size, strtol_family);
  hle("strtod", import_strtod);
  hle("qsort", import_qsort);

  // Wide characters: bionic's wchar_t is 32-bit on both ABIs, so these forward as-is.
  gen("wcscpy", "pp>P");
  gen("wcscmp", "pp>i");
  gen("wcslen", "p>u");
  gen("wcschr", "pi>P");
  gen("wcsrchr", "pi>P");
  gen("wcsncpy", "ppu>P");
  gen("wcscat", "pp>P");
  gen("wcsncmp", "ppu>i");
  gen("wmemcpy", "ppu>P");
  gen("wmemmove", "ppu>P");
  gen("wmemset", "piu>P");
  gen("wmemcmp", "ppu>i");
  gen("wmemchr", "piu>P");
  gen("wcsspn", "pp>u");
  gen("wcscspn", "pp>u");
  gen("wcscoll", "pp>i");
  gen("wcsxfrm", "ppu>u");
  gen("wcstombs", "ppu>u");
  gen("wcrtomb", "pup>u");
  gen("mbrtowc", "ppup>u");
  gen("btowc", "i>u");
  gen("wctob", "i>i");
  gen("towlower", "i>i");
  gen("towupper", "i>i");
  gen("wctype", "p>u");
  gen("iswctype", "ii>i");
  for (const char* size : {"iswalpha", "iswdigit", "iswspace", "iswupper", "iswlower", "iswalnum", "iswpunct",
                           "iswprint", "iswxdigit", "iswcntrl"})
    gen(size, "i>i");
  hle("wcstol", import_wcstol);
  for (const char* name : {"isxdigit", "islower"})
    gen(name, "i>i");
}
