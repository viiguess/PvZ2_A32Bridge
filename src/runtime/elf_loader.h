// Minimal ELF32 (ARM) loader: maps PT_LOAD segments at IMG_BASE and relocates against the import stubs.
#pragma once

#include <string>
#include <vector>

#include "common.h"

struct Image {
  std::string path;                // file it was loaded from
  u32 base = 0, size = 0;
  u32 exidx = 0, exidx_size = 0;  // ARM exception index table (PT_ARM_EXIDX)
  std::vector<u32> init;          // DT_INIT and DT_INIT_ARRAY entries, in call order
};

bool load_elf(const char* path, Image& image);
u32 image_sym(const Image& image, const char* name);  // guest address of an exported symbol, 0 if absent
extern Image g_main;                                  // the game library
