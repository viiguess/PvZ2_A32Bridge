#include "runtime/mods.h"

#include <string.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "libc/system_imports.h"

static std::vector<Image*> g_mods;
// In the order the game's Java code loads them.
static const char* const MOD_NAMES[] = {"czProject", "HookPVZ2", "Restart"};

// dladdr that also knows the mods. Dl_info of 32-bit bionic: { fname, fbase, sname, saddr }.
static void import_dladdr(Ctx& ctx, const Stub&) {
  u32 address = ctx.a(0);
  u32* info = (u32*)ctx.p(1);
  const Image* found = nullptr;
  if (address >= g_main.base && address < g_main.base + g_main.size)
    found = &g_main;
  for (const Image* mod : g_mods)
    if (address >= mod->base && address < mod->base + mod->size)
      found = mod;
  if (!found) {
    info[0] = info[1] = info[2] = info[3] = 0;
    ctx.ret(0);
    return;
  }
  static std::mutex mutex;
  static std::map<const Image*, u32> names;  // guest copies of lib<x>.so
  std::lock_guard<std::mutex> lock(mutex);
  u32& name = names[found];
  if (!name)
    name = gstr(shown_name(found->path).c_str());
  info[0] = name;
  info[1] = found->base;
  info[2] = info[3] = 0;
  ctx.ret(1);
}

static void import_sigemptyset(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(0) = 0;  // sigset_t of 32-bit bionic is one word
  ctx.ret(0);
}

void mods_register_imports() {
  stub_add("sigemptyset", import_sigemptyset);
  stub_add("dladdr", import_dladdr);
}

void mods_load(const std::string& directory, u32 guest_java_vm) {
  u32 next = MOD_BASE;
  for (const char* name : MOD_NAMES) {
    std::string path = directory + "/lib" + name + "_a32.so";
    if (access(path.c_str(), R_OK) != 0)
      continue;
    Image* image = new Image;
    if (!load_elf(path.c_str(), *image, next)) {
      LOG("mod %s: cannot load", name);
      delete image;
      continue;
    }
    next += (image->size + 0xfffff) & ~0xfffffu;
    g_mods.push_back(image);
    fake_proc_maps_add(*image);  // before its constructors: they may read /proc/self/maps
    LOG("mod %s loaded at %08x (%zu init fns)", name, image->base, image->init.size());
    for (u32 constructor : image->init)
      guest_callv(constructor, nullptr, 0, nullptr);
    u32 on_load = image_sym(*image, "JNI_OnLoad");
    if (on_load) {
      u32 args[2] = {guest_java_vm, 0};
      [[maybe_unused]] u32 result = guest_callv(on_load, args, 2, nullptr);
      LOG("mod %s: JNI_OnLoad -> %08x", name, result);
    }
  }
}
