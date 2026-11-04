// Minimal ELF32 (ARM) loader: maps PT_LOAD segments at IMG_BASE and applies relocations against the import stubs.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "runtime/elf_format.h"
#include "runtime/runtime.h"

Image g_main;

// Symbol table of a loaded image, found through its (already mapped) PT_DYNAMIC segment.
struct DynInfo {
  const Sym* symbols = nullptr;
  const char* strings = nullptr;
  u32 symbol_count = 0;
};

// The image's own program headers are part of its first PT_LOAD segment, so they can be read back from guest memory.
static const Phdr* program_headers(const Image& image, const Ehdr** ehdr) {
  *ehdr = (const Ehdr*)(G + image.base);
  return (const Phdr*)(G + image.base + (*ehdr)->phoff);
}

static DynInfo read_dynamic_symbols(const Image& image) {
  DynInfo info;
  const Ehdr* ehdr;
  const Phdr* phdr = program_headers(image, &ehdr);
  for (int i = 0; i < ehdr->phnum; i++) {
    if (phdr[i].type != PT_DYNAMIC)
      continue;
    u32 hash_table = 0;
    for (const Dyn* dyn = (const Dyn*)(G + image.base + phdr[i].vaddr); dyn->tag != DT_NULL; dyn++) {
      if (dyn->tag == DT_SYMTAB)
        info.symbols = (const Sym*)(G + image.base + dyn->val);
      if (dyn->tag == DT_STRTAB)
        info.strings = (const char*)(G + image.base + dyn->val);
      if (dyn->tag == DT_HASH)
        hash_table = dyn->val;
    }
    if (hash_table)
      info.symbol_count = ((u32*)(G + image.base + hash_table))[1];  // nchain == number of symbols
  }
  return info;
}

u32 image_sym(const Image& image, const char* name) {
  DynInfo info = read_dynamic_symbols(image);
  for (u32 i = 0; i < info.symbol_count; i++) {
    if (info.symbols[i].shndx && !strcmp(info.strings + info.symbols[i].name, name))
      return image.base + info.symbols[i].value;
  }
  return 0;
}

static bool read_file(const char* path, std::vector<uint8_t>& out) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    LOG("cannot open %s", path);
    return false;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out.resize(size);
  bool ok = fread(out.data(), 1, size, file) == (size_t)size;
  fclose(file);
  return ok;
}

// Applies a table of REL relocations (no addends: the addend is the value already stored at the target).
static void apply_relocations(const Image& image, const DynInfo& symbols, u32 table, u32 table_size) {
  const Rel* rel = (const Rel*)(G + image.base + table);
  for (u32 i = 0; i < table_size / sizeof(Rel); i++) {
    u32* target = (u32*)(G + image.base + rel[i].offset);
    u32 type = rel[i].info & 0xff, symbol_index = rel[i].info >> 8, symbol_value = 0;
    if (type == R_ARM_RELATIVE) {
      *target += image.base;
      continue;
    }
    if (symbol_index) {
      const Sym& sym = symbols.symbols[symbol_index];
      const char* name = symbols.strings + sym.name;
      if (sym.shndx) {  // defined in this image
        symbol_value = image.base + sym.value;
      } else {  // import: resolve to a stub (unknown C++ symbols are expected and stay silent)
        symbol_value = stub_find(name);
        if (!symbol_value) {
          symbol_value = stub_add(strdup(name), nullptr);
          if (strncmp(name, "_Z", 2))
            LOG("unresolved import %s", name);
        }
      }
    }
    if (type == R_ARM_ABS32)
      *target += symbol_value;
    else if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT)
      *target = symbol_value;
    else
      LOG("unsupported reloc type %u", type);
  }
}

bool load_elf(const char* path, Image& image) {
  std::vector<uint8_t> file;
  if (!read_file(path, file))
    return false;
  const Ehdr* ehdr = (const Ehdr*)file.data();
  if (memcmp(ehdr->id,
             "\x7f"
             "ELF",
             4) ||
      ehdr->machine != EM_ARM || ehdr->id[4] != ELFCLASS32) {
    LOG("not an ARM32 ELF");
    return false;
  }
  const Phdr* phdr = (const Phdr*)(file.data() + ehdr->phoff);

  // Map the loadable segments.
  u32 image_end = 0;
  for (int i = 0; i < ehdr->phnum; i++) {
    if (phdr[i].type == PT_LOAD && phdr[i].vaddr + phdr[i].memsz > image_end)
      image_end = phdr[i].vaddr + phdr[i].memsz;
  }
  image.base = IMG_BASE;
  image.size = (image_end + 0xfff) & ~0xfffu;
  for (int i = 0; i < ehdr->phnum; i++) {
    if (phdr[i].type == PT_LOAD)
      memcpy(G + image.base + phdr[i].vaddr, file.data() + phdr[i].offset, phdr[i].filesz);
    if (phdr[i].type == PT_ARM_EXIDX) {
      image.exidx = image.base + phdr[i].vaddr;
      image.exidx_size = phdr[i].memsz;
    }
  }

  // Collect what the dynamic section says about relocations and constructors.
  DynInfo symbols = read_dynamic_symbols(image);
  const Dyn* dyn = nullptr;
  for (int i = 0; i < ehdr->phnum; i++) {
    if (phdr[i].type == PT_DYNAMIC)
      dyn = (const Dyn*)(G + image.base + phdr[i].vaddr);
  }
  u32 rel = 0, rel_size = 0, jmprel = 0, jmprel_size = 0, init_array = 0, init_array_size = 0, init = 0;
  for (; dyn && dyn->tag != DT_NULL; dyn++) {
    switch (dyn->tag) {
      case DT_REL:
        rel = dyn->val;
        break;
      case DT_RELSZ:
        rel_size = dyn->val;
        break;
      case DT_JMPREL:
        jmprel = dyn->val;
        break;
      case DT_PLTRELSZ:
        jmprel_size = dyn->val;
        break;
      case DT_INIT_ARRAY:
        init_array = dyn->val;
        break;
      case DT_INIT_ARRAYSZ:
        init_array_size = dyn->val;
        break;
      case DT_INIT:
        init = dyn->val;
        break;
    }
  }

  if (rel)
    apply_relocations(image, symbols, rel, rel_size);
  if (jmprel)
    apply_relocations(image, symbols, jmprel, jmprel_size);

  // Constructors: DT_INIT first, then DT_INIT_ARRAY (entries of 0 and -1 are placeholders).
  if (init)
    image.init.push_back(image.base + init);
  if (init_array) {
    for (u32 i = 0; i < init_array_size / sizeof(u32); i++) {
      u32 fn = ((u32*)(G + image.base + init_array))[i];
      if (fn && fn != 0xffffffff)
        image.init.push_back(fn);
    }
  }
  return true;
}
