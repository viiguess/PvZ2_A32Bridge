// stdio, files, directories, sockets, and guest stdout/stderr redirected to logcat.
#pragma once

#include <stdio.h>

#include <string>

#include "runtime/runtime.h"

void register_stdio_imports();

FILE* file_host(u32 guest_file);  // host FILE behind a guest FILE* (including stdin/stdout/stderr)
u32 file_new(FILE* host_file);    // wrap a host FILE in a guest FILE object
void log_out(int prio, const char* tag, const std::string& text);

// Optional: lets an embedding program rewrite the paths the guest opens (open, fopen, opendir), for example to serve
// a synthetic /proc/self/maps. Returns the path to use; null hook = identity.
extern const char* (*g_path_hook)(const char*);
