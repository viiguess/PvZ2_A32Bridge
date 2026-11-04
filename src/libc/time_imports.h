// time, date and calendar imports.
#pragma once

#include <time.h>

#include "runtime/runtime.h"

void register_time_imports();

timespec timespec_from_guest(u32 guest_timespec);  // 32-bit timespec -> host timespec
