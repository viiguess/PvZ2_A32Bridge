// Debug aid: writes guest code to a file when the guest flushes a large freshly written range (a loader unpacking its
// payload, e.g. Marmalade's .s3e), so it can be disassembled. Enabled only when the app's files directory contains a
// file named a32_dump_code; the dump goes next to it as a32_code_<guest address>.bin.
#pragma once

#include "common.h"

void code_dump_on_flush(u32 start, u32 size);
