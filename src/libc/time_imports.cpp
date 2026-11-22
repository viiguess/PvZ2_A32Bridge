// time, date and locale-independent calendar imports.
#include "libc/time_imports.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"

timespec timespec_from_guest(u32 guest_addr) {
  timespec time_value;
  time_value.tv_sec = (int32_t)((u32*)H(guest_addr))[0];
  time_value.tv_nsec = ((u32*)H(guest_addr))[1];
  return time_value;
}

static void tm_to_guest(const struct tm* time_value, u32 guest_addr) {
  int32_t* out = (int32_t*)H(guest_addr);
  out[0] = time_value->tm_sec;
  out[1] = time_value->tm_min;
  out[2] = time_value->tm_hour;
  out[3] = time_value->tm_mday;
  out[4] = time_value->tm_mon;
  out[5] = time_value->tm_year;
  out[6] = time_value->tm_wday;
  out[7] = time_value->tm_yday;
  out[8] = time_value->tm_isdst;
  out[9] = (int32_t)time_value->tm_gmtoff;
  ((u32*)out)[10] = time_value->tm_zone ? gstr(time_value->tm_zone) : 0;
}

static void tm_from_guest(u32 guest_addr, struct tm* time_value) {
  memset(time_value, 0, sizeof *time_value);
  int32_t* out = (int32_t*)H(guest_addr);
  time_value->tm_sec = out[0];
  time_value->tm_min = out[1];
  time_value->tm_hour = out[2];
  time_value->tm_mday = out[3];
  time_value->tm_mon = out[4];
  time_value->tm_year = out[5];
  time_value->tm_wday = out[6];
  time_value->tm_yday = out[7];
  time_value->tm_isdst = out[8];
  time_value->tm_gmtoff = out[9];
}

static void import_time(Ctx& ctx, const Stub&) {
  time_t time_value = time(nullptr);
  if (ctx.a(0))
    *(u32*)ctx.p(0) = (u32)time_value;
  ctx.ret((u32)time_value);
}

static void import_gettimeofday(Ctx& ctx, const Stub&) {
  timeval now;
  gettimeofday(&now, nullptr);
  if (ctx.a(0)) {
    ((u32*)ctx.p(0))[0] = (u32)now.tv_sec;
    ((u32*)ctx.p(0))[1] = (u32)now.tv_usec;
  }
  ctx.ret(0);
}

static void import_clock_gettime(Ctx& ctx, const Stub&) {
  timespec time_value;
  int result = clock_gettime((clockid_t)ctx.a(0), &time_value);
  if (ctx.a(1)) {
    ((u32*)ctx.p(1))[0] = (u32)time_value.tv_sec;
    ((u32*)ctx.p(1))[1] = (u32)time_value.tv_nsec;
  }
  ctx.ret(result);
}

static void import_nanosleep(Ctx& ctx, const Stub&) {
  timespec time_value = timespec_from_guest(ctx.a(0));
  ctx.ret(nanosleep(&time_value, nullptr));
}

static u32 g_tm_buf, g_ctime_buf;

static void localtime_gmtime(Ctx& ctx, const Stub& stub) {
  time_t time_value = (int32_t) * (u32*)ctx.p(0);
  struct tm calendar;
  if (!strcmp(stub.name, "gmtime"))
    gmtime_r(&time_value, &calendar);
  else
    localtime_r(&time_value, &calendar);
  tm_to_guest(&calendar, g_tm_buf);
  ctx.ret(g_tm_buf);
}

static void localtime_gmtime_r(Ctx& ctx, const Stub& stub) {  // (const time_t*, struct tm*)
  time_t time_value = (int32_t) * (u32*)ctx.p(0);
  struct tm calendar;
  if (!strcmp(stub.name, "gmtime_r"))
    gmtime_r(&time_value, &calendar);
  else
    localtime_r(&time_value, &calendar);
  tm_to_guest(&calendar, ctx.a(1));
  ctx.ret(ctx.a(1));
}

static void import_clock_getres(Ctx& ctx, const Stub&) {
  timespec resolution;
  int result = clock_getres((clockid_t)ctx.a(0), &resolution);
  if (ctx.a(1)) {
    ((u32*)ctx.p(1))[0] = (u32)resolution.tv_sec;
    ((u32*)ctx.p(1))[1] = (u32)resolution.tv_nsec;
  }
  ctx.ret(result);
}

static void import_mktime(Ctx& ctx, const Stub&) {
  struct tm calendar;
  tm_from_guest(ctx.a(0), &calendar);
  time_t time_value = mktime(&calendar);
  tm_to_guest(&calendar, ctx.a(0));
  ctx.ret((u32)time_value);
}

static void import_ctime(Ctx& ctx, const Stub&) {
  time_t time_value = (int32_t) * (u32*)ctx.p(0);
  ctime_r(&time_value, (char*)H(g_ctime_buf));
  ctx.ret(g_ctime_buf);
}

static void import_strftime(Ctx& ctx, const Stub&) {
  struct tm calendar;
  tm_from_guest(ctx.a(3), &calendar);
  ctx.ret((u32)strftime((char*)ctx.p(0), ctx.a(1), (const char*)ctx.p(2), &calendar));
}

static void import_strptime(Ctx& ctx, const Stub&) {  // (const char* s, const char* format, struct tm*)
  struct tm calendar;
  tm_from_guest(ctx.a(2), &calendar);
  const char* input = (const char*)ctx.p(0);
  const char* end = strptime(input, (const char*)ctx.p(1), &calendar);
  if (!end) {
    ctx.ret(0);
    return;
  }
  tm_to_guest(&calendar, ctx.a(2));
  ctx.ret(ctx.a(0) + (u32)(end - input));
}

static void import_asctime(Ctx& ctx, const Stub&) {
  static u32 buf = gdata_alloc(32, 4);
  struct tm calendar;
  tm_from_guest(ctx.a(0), &calendar);
  char time_value[32];
  asctime_r(&calendar, time_value);
  strcpy((char*)H(buf), time_value);
  ctx.ret(buf);
}

static void import_wcsftime(Ctx& ctx,
                            const Stub&) {  // (wchar_t*, size_t, const wchar_t* fmt, const tm*); formats are ASCII
  std::string format;
  for (const u32* w = (const u32*)ctx.p(2); *w; w++)
    format += (char)*w;
  struct tm calendar;
  tm_from_guest(ctx.a(3), &calendar);
  std::string out(512, 0);
  size_t written = strftime(&out[0], out.size(), format.c_str(), &calendar);
  u32 cap = ctx.a(1), count = 0;
  u32* dest = (u32*)ctx.p(0);
  if (written >= cap) {
    ctx.ret(0);
    return;
  }
  for (; count < written; count++)
    dest[count] = (uint8_t)out[count];
  dest[count] = 0;
  ctx.ret(count);
}

void register_time_imports() {
  g_tm_buf = gdata_alloc(48, 8);
  g_ctime_buf = gdata_alloc(32, 8);
  hle("time", import_time);
  hle("gettimeofday", import_gettimeofday);
  hle("clock_gettime", import_clock_gettime);
  hle("nanosleep", import_nanosleep);
  hle("localtime", localtime_gmtime);
  hle("localtime_r", localtime_gmtime_r);
  hle("gmtime_r", localtime_gmtime_r);
  hle("clock_getres", import_clock_getres);
  hle("gmtime", localtime_gmtime);
  hle("mktime", import_mktime);
  hle("ctime", import_ctime);
  hle("strftime", import_strftime);
  hle("strptime", import_strptime);
  hle("asctime", import_asctime);
  hle("wcsftime", import_wcsftime);
}
