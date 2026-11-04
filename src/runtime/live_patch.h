// Optional runtime patches applied to the guest library right after it is mapped (before constructors run).
// Patches are located by instruction shape, not fixed offsets, and are skipped unless every site is found.
#pragma once

#include "elf_loader.h"

// Returns the number of patch sets applied. Compiled to a no-op unless built with -DA32_FREE_SHOP=ON.
int apply_live_patches(const Image& image);
