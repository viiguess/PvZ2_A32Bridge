// Minimal <android/log.h> so that the shared runtime sources build on a Linux host: messages go to stderr.
#pragma once

#include <stdio.h>

enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_ERROR = 6, ANDROID_LOG_FATAL = 7 };

#define __android_log_print(prio, tag, ...) \
  (fprintf(stderr, "[%s] ", tag), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr), 0)

static inline int __android_log_write(int prio, const char* tag, const char* text) {
  return fprintf(stderr, "[%s] %s\n", tag, text);
}
