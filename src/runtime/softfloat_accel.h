// Soft-float acceleration. armeabi games do every float operation through libgcc helpers (__aeabi_fadd, ...) that
// emulate IEEE arithmetic with integer code. They are replaced by VFP versions (tools/softfloat_vfp.S), which the JIT
// turns into single host FP instructions. Same results, several times faster.
#pragma once

#include "runtime/elf_loader.h"

// Redirects the helpers `image` exports to the VFP versions. Call after load_elf, before the image runs.
int accelerate_soft_float(const Image& image);

// Exports the VFP versions as imports, for games linked against an old libc that provided the helpers.
void export_soft_float_helpers();
