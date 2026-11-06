#include "runtime/softfloat_accel.h"

#include <string.h>

#include <set>

#include "runtime/runtime.h"
#include "runtime/softfloat_vfp.h"

void export_soft_float_helpers() {
  u32 code = sym_add_asm("__a32_softfloat_vfp", kSoftFloatVfpCode, sizeof kSoftFloatVfpCode / 4);
  for (const SoftFloatVfpEntry& entry : kSoftFloatVfpEntries)
    sym_add(entry.name, code + entry.offset);
}

int accelerate_soft_float(const Image& image) {
  // The first page after the image is within reach of an ARM `b` (+-32 MiB) from every helper.
  u32 code = (image.base + image.size + 0xfff) & ~0xfffu;
  bool placed = false;
  std::set<u32> patched;
  for (const SoftFloatVfpEntry& entry : kSoftFloatVfpEntries) {
    u32 at = image_sym(image, entry.name);
    if (!at || (at & 3) || patched.count(at))  // absent, Thumb, or an alias already redirected
      continue;
    int64_t distance = (int64_t)(code + entry.offset) - (int64_t)(at + 8);
    if (distance < -(1 << 25) || distance >= (1 << 25))
      continue;
    if (!placed) {
      memcpy(G + code, kSoftFloatVfpCode, sizeof kSoftFloatVfpCode);
      placed = true;
    }
    *(u32*)(G + at) = 0xEA000000u | ((u32)(distance >> 2) & 0x00FFFFFFu);  // b <replacement>
    patched.insert(at);
  }
  if (!patched.empty())
    LOG("soft-float: %zu libgcc helper(s) redirected to VFP code at %08x", patched.size(), code);
  return (int)patched.size();
}
