// What a Linux host has to fake so that an Android library can run in it: libc functions the game library never
// needed, a synthetic /proc/self/maps, and raw Linux system calls.
#pragma once

// Registers imports that the bridge's libc layer lacks (mmap, strdup, uname, rwlocks, ...) and the path hook.
// Call after libc_init() and before the library is loaded.
void register_host_imports();

// Writes the file that /proc/self/maps is redirected to: the library itself plus a plausible set of system libraries.
// Call after the library is loaded (its address range is needed).
void write_fake_maps();

// Emulates the Linux system calls the guest issues with a raw `svc #0` (syscalls.cpp).
void install_syscalls();
