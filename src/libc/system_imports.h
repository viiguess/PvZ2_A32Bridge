// Process, memory-mapping and dynamic-loader imports that packers and SDK libraries use but games rarely do, and the
// parts of Android's runtime a packer inspects (/proc/self/maps, raw system calls, ART's DexFile).
#pragma once

#include <string>

#include "common.h"

void register_system_imports();

// Serves a synthetic /proc/self/maps to the guest: its own image plus a 32-bit system (linker, libc, libart). Packers
// read the real file to look for emulators and debuggers, and a 64-bit process looks like neither a 32-bit device nor
// a normal one. Call after the guest library is loaded.
void install_fake_proc_maps();
// Lists a companion library (a mod loaded next to the guest) in the synthetic /proc/self/maps as well.
struct Image;
void fake_proc_maps_add(const Image& image);
void fake_proc_maps_add_region(u32 base, u32 size, const std::string& name);  // a non-library mapping (e.g. a dex)
// The name the guest sees for a library path: lib<name>_a32.so -> lib<name>.so.
std::string shown_name(const std::string& path);

// Emulates the Linux system calls the guest issues with a raw `svc #0` (syscalls.cpp), for code that bypasses libc.
void install_syscalls();

// A packer hands its decrypted dex to ART through art::DexFile::OpenMemory (found by mangled name) and then reads
// fields of the result. The registered stubs return this object: a DexFile whose virtual methods all return 0 and
// whose first fields point at the dex. fake_dex_file() builds it for a dex at `base` with `size` bytes.
u32 fake_dex_file(u32 base, u32 size);
// Called with every dex the packer hands over (pointer into guest memory, size). The Android bridge sets it to load the
// classes into the app's class loader, because it cannot register the dex with ART the way the packer does.
extern void (*g_dex_handover_hook)(const uint8_t* dex, u32 size);
