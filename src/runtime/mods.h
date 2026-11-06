// Companion hook libraries ("mods", e.g. czProject) that patch the game library at run time. They run in the guest
// next to it; they find the game through /proc/self/maps and flush patched code with the cacheflush syscall.
#pragma once

#include <string>

#include "runtime/runtime.h"

void mods_register_imports();                                     // from libc_init, after the other imports
void mods_load(const std::string& directory, u32 guest_java_vm);  // lib<name>_a32.so files next to the bridge
