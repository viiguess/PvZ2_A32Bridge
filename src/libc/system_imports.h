// Process, memory-mapping and dynamic-loader imports that packers and SDK libraries use but games rarely do, and the
// parts of Android's runtime a packer inspects (/proc/self/maps, raw system calls, ART's DexFile).
#pragma once

#include "common.h"

void register_system_imports();

// Serves a synthetic /proc/self/maps to the guest: its own image plus a 32-bit system (linker, libc, libart). Packers
// read the real file to look for emulators and debuggers, and a 64-bit process looks like neither a 32-bit device nor
// a normal one. Call after the guest library is loaded.
void install_fake_proc_maps();

// Emulates the Linux system calls the guest issues with a raw `svc #0` (syscalls.cpp), for code that bypasses libc.
void install_syscalls();

// A packer hands its decrypted dex to ART through art::DexFile::OpenMemory (found by mangled name) and then reads
// fields of the result. The registered stubs return this object: a DexFile whose virtual methods all return 0 and
// whose first fields point at the dex. fake_dex_file() builds it for a dex at `base` with `size` bytes.
u32 fake_dex_file(u32 base, u32 size);
