// stdio, file and directory imports, plus guest-visible stdout/stderr redirected to logcat.
#include "libc/stdio_imports.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/format.h"
#include "libc/forward.h"
#include "libc/guest_abi.h"
#include "libc/log_relay.h"

// ---------------------------------------------------------------------------------------------------------------
// Guest FILE objects (layout in guest_abi.h). stdin/stdout/stderr map to the host streams; with logging enabled,
// output to stdout/stderr goes to logcat.

static u32 g_std_streams;  // guest address of __sF[GUEST_STD_STREAMS]

static StdStream std_stream_of(u32 guest_file) {
  if (guest_file < g_std_streams || guest_file >= g_std_streams + GUEST_STD_STREAMS * GUEST_FILE_SIZE)
    return NOT_STD;
  return (StdStream)((guest_file - g_std_streams) / GUEST_FILE_SIZE);
}

FILE* file_host(u32 guest_file) {
  if (!guest_file)
    return nullptr;
  switch (std_stream_of(guest_file)) {
    case STD_IN:
      return stdin;
    case STD_OUT:
      return stdout;
    case STD_ERR:
      return stderr;
    default:
      return *(FILE**)H(guest_file);
  }
}

u32 file_new(FILE* host_file) {
  if (!host_file)
    return 0;
  u32 guest_file = gmalloc(GUEST_FILE_SIZE);
  memset(H(guest_file), 0, GUEST_FILE_SIZE);
  *(FILE**)H(guest_file) = host_file;
  return guest_file;
}

void log_out(int prio, const char* tag, const std::string& text) {
#ifdef A32_LOG
  __android_log_write(prio, tag, text.c_str());
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// Struct conversions (32-bit bionic layouts)

// struct stat as laid out by 32-bit bionic (104 bytes).
static void stat_to_guest(const struct stat& info, u32 guest_addr) {
  uint8_t* out = (uint8_t*)H(guest_addr);
  memset(out, 0, STAT_STRUCT_SIZE);
  auto put32 = [&](int offset, u32 v) { memcpy(out + offset, &v, 4); };
  auto put64 = [&](int offset, u64 v) { memcpy(out + offset, &v, 8); };
  put64(STAT_DEV, info.st_dev);
  put32(STAT_INO32, (u32)info.st_ino);
  put32(STAT_MODE, info.st_mode);
  put32(STAT_NLINK, (u32)info.st_nlink);
  put32(STAT_UID, info.st_uid);
  put32(STAT_GID, info.st_gid);
  put64(STAT_RDEV, info.st_rdev);
  put64(STAT_SIZE, info.st_size);
  put32(STAT_BLKSIZE, (u32)info.st_blksize);
  put64(STAT_BLOCKS, info.st_blocks);
  put32(STAT_ATIME, (u32)info.st_atim.tv_sec);
  put32(STAT_ATIME + 4, (u32)info.st_atim.tv_nsec);
  put32(STAT_MTIME, (u32)info.st_mtim.tv_sec);
  put32(STAT_MTIME + 4, (u32)info.st_mtim.tv_nsec);
  put32(STAT_CTIME, (u32)info.st_ctim.tv_sec);
  put32(STAT_CTIME + 4, (u32)info.st_ctim.tv_nsec);
  put64(STAT_INO64, info.st_ino);
}

// open(2) flags differ between arm32 and arm64 for these four.
static int open_flags_from_guest(u32 flags) {
  int host = flags & ~(ARM32_O_DIRECTORY | ARM32_O_NOFOLLOW | ARM32_O_DIRECT | ARM32_O_LARGEFILE);
  if (flags & ARM32_O_DIRECTORY)
    host |= O_DIRECTORY;
  if (flags & ARM32_O_NOFOLLOW)
    host |= O_NOFOLLOW;
  if (flags & ARM32_O_DIRECT)
    host |= O_DIRECT;
  return host;
}

static void import_stat(Ctx& ctx, const Stub&) {
  struct stat info;
  int result = stat(guest_host_path((const char*)ctx.p(0)), &info);
  set_errno();
  if (result == 0)
    stat_to_guest(info, ctx.a(1));
  ctx.ret(result);
}

static void import_statfs(Ctx& ctx, const Stub&) {
  struct statfs info;
  int result = statfs(guest_host_path((const char*)ctx.p(0)), &info);
  set_errno();
  if (result == 0) {
    uint8_t* out = (uint8_t*)H(ctx.a(1));
    memset(out, 0, 88);
    u32 head[2] = {(u32)info.f_type, (u32)info.f_bsize};
    memcpy(out, head, 8);
    u64 counts[3] = {(u64)info.f_blocks, (u64)info.f_bfree, (u64)info.f_bavail};
    memcpy(out + 8, counts, 24);
    u32 frame_size = (u32)info.f_frsize;
    memcpy(out + 76, &frame_size, 4);
  }
  ctx.ret(result);
}

const char* (*g_path_hook)(const char*) = nullptr;

// System files old games open by path that current Android no longer ships: the CJK font was DroidSansFallback
// until Android 6 and is a Noto CJK collection now.
static const char* legacy_system_path(const char* path) {
  static const char* const kOldCjkFonts[] = {"/system/fonts/DroidSansFallback.ttf",
                                             "/system/fonts/DroidSansFallbackFull.ttf"};
  static const char* const kCjkFonts[] = {
      "/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansSC-Regular.otf",
      "/system/fonts/NotoSansTC-Regular.otf", "/system/fonts/NotoSansCJKsc-Regular.otf"};
  for (const char* old_font : kOldCjkFonts) {
    if (strcmp(path, old_font) != 0 || access(path, R_OK) == 0)
      continue;
    for (const char* font : kCjkFonts)
      if (access(font, R_OK) == 0)
        return font;
  }
  return path;
}

// The game keeps its profile and level data in <app dir>/filesNo_Backup. The folder is served from the app's own
// directory on shared storage (Android/data/<package>/), where a file manager or a USB connection can reach it.
// Data already in the internal folder is copied across once and the internal folder is renamed to <name>.migrated.
namespace {

struct SaveRedirect {
  bool enabled = false;
  std::string external;                // /storage/emulated/<user>/Android/data/<package>
  std::vector<std::string> internals;  // the internal app directory under each of its names
};

const char* const kSaveFolders[] = {"filesNo_Backup", "No_Backup", "no_backup"};

bool make_dirs(const std::string& path) {
  for (size_t at = 1; at <= path.size(); at++) {
    if (at == path.size() || path[at] == '/') {
      std::string part = path.substr(0, at);
      if (mkdir(part.c_str(), 0770) != 0 && errno != EEXIST && access(part.c_str(), F_OK) != 0)
        return false;
    }
  }
  return true;
}

void copy_tree(const std::string& from, const std::string& to) {
  make_dirs(to);
  DIR* dir = opendir(from.c_str());
  if (!dir)
    return;
  while (dirent* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    std::string source = from + "/" + name, target = to + "/" + name;
    struct stat info;
    if (stat(source.c_str(), &info) != 0)
      continue;
    if (S_ISDIR(info.st_mode)) {
      copy_tree(source, target);
    } else if (S_ISREG(info.st_mode)) {
      FILE* in = fopen(source.c_str(), "rb");
      FILE* out = in ? fopen(target.c_str(), "wb") : nullptr;
      if (in && out) {
        char buffer[16384];
        size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, in)) > 0)
          fwrite(buffer, 1, count, out);
      }
      if (out)
        fclose(out);
      if (in)
        fclose(in);
    }
  }
  closedir(dir);
}

SaveRedirect& save_redirect() {
  static SaveRedirect redirect;
  static std::once_flag once;
  std::call_once(once, [] {
    char name[256] = {0};
    FILE* cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
      size_t length = fread(name, 1, sizeof name - 1, cmdline);
      name[length] = 0;
      fclose(cmdline);
    }
    std::string package = name;
    package = package.substr(0, package.find(':'));
    if (package.find('.') == std::string::npos || package.find('/') != std::string::npos)
      return;

    int user = (int)(getuid() / 100000);
    std::string external = "/storage/emulated/" + std::to_string(user) + "/Android/data/" + package;
    if (!make_dirs(external) || access(external.c_str(), W_OK) != 0)
      return;

    SaveRedirect& r = redirect;
    r.external = external;
    r.internals = {"/data/data/" + package, "/data/user/" + std::to_string(user) + "/" + package,
                   "/data/user/0/" + package};
    for (const char* folder : kSaveFolders) {
      for (const std::string& base : r.internals) {
        std::string internal = base + "/" + folder, target = external + "/" + folder;
        struct stat info;
        if (stat(internal.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
          continue;
        if (access(target.c_str(), F_OK) != 0)
          copy_tree(internal, target);
        rename(internal.c_str(), (internal + ".migrated").c_str());
        break;
      }
    }
    for (const char* folder : kSaveFolders)
      make_dirs(external + "/" + folder);
    r.enabled = true;
  });
  return redirect;
}

// The shared-storage path for a path inside one of the save folders, or null.
const char* redirected_save_path(const char* path) {
  if (path[0] != '/' || strncmp(path, "/data/", 6) != 0)
    return nullptr;
  SaveRedirect& r = save_redirect();
  if (!r.enabled)
    return nullptr;
  for (const std::string& base : r.internals) {
    if (strncmp(path, base.c_str(), base.size()) != 0 || path[base.size()] != '/')
      continue;
    const char* rest = path + base.size() + 1;
    for (const char* folder : kSaveFolders) {
      size_t length = strlen(folder);
      if (strncmp(rest, folder, length) == 0 && (rest[length] == 0 || rest[length] == '/')) {
        static thread_local std::string redirected;
        redirected = r.external + "/" + rest;
        return redirected.c_str();
      }
    }
  }
  return nullptr;
}

}  // namespace

const char* guest_host_path(const char* guest_path) {
  if (const char* redirected = redirected_save_path(guest_path))
    return redirected;
  return legacy_system_path(g_path_hook ? g_path_hook(guest_path) : guest_path);
}

static void import_access(Ctx& ctx, const Stub&) {
  int result = access(guest_host_path((const char*)ctx.p(0)), (int)ctx.a(1));
  set_errno();
  ctx.ret(result);
}

static void import_open(Ctx& ctx, const Stub&) {
  int result = open(guest_host_path((const char*)ctx.p(0)), open_flags_from_guest(ctx.a(1)), ctx.a(2));
  if (result < 0 && errno == ENOENT) {
    // Old Jiagu packers expect the oat file their dexopt child creates next to the unpacked dex.
    const char* guest_path = (const char*)ctx.p(0);
    size_t length = strlen(guest_path);
    if (length > 17 && !strcmp(guest_path + length - 17, "/.jiagu/classes.oat"))
      result = open(guest_host_path(guest_path), open_flags_from_guest(ctx.a(1)) | O_CREAT, 0600);
  }
  set_errno();
  ctx.ret(result);
}

static void import_utime(Ctx& ctx, const Stub&) {
  int result;
  if (ctx.a(1)) {
    utimbuf times;
    times.actime = (int32_t)((u32*)ctx.p(1))[0];
    times.modtime = (int32_t)((u32*)ctx.p(1))[1];
    result = utime(guest_host_path((const char*)ctx.p(0)), &times);
  } else
    result = utime(guest_host_path((const char*)ctx.p(0)), nullptr);
  set_errno();
  ctx.ret(result);
}

static void import_select(Ctx& ctx, const Stub&) {
  timeval timeout, *ptv = nullptr;
  if (ctx.a(4)) {
    timeout.tv_sec = (int32_t)((u32*)ctx.p(4))[0];
    timeout.tv_usec = (int32_t)((u32*)ctx.p(4))[1];
    ptv = &timeout;
  }
  int result = select(ctx.a(0), (fd_set*)ctx.p(1), (fd_set*)ctx.p(2), (fd_set*)ctx.p(3), ptv);
  set_errno();
  ctx.ret(result);
}

static void import_fsetpos(Ctx& ctx, const Stub&) {
#ifdef A32_HOST_RUNNER  // glibc's fpos_t is a struct; bionic's is the offset
  ctx.ret(fseek(file_host(ctx.a(0)), *(int32_t*)ctx.p(1), SEEK_SET));
#else
  fpos_t position = *(int32_t*)ctx.p(1);
  ctx.ret(fsetpos(file_host(ctx.a(0)), &position));
#endif
}

// FILE and dir
static void import_fopen(Ctx& ctx, const Stub&) {
  FILE* file = fopen(guest_host_path((const char*)ctx.p(0)), (const char*)ctx.p(1));
  set_errno();
  ctx.ret(file_new(file));
}

static void import_fclose(Ctx& ctx, const Stub&) {
  u32 guest_file = ctx.a(0);
  if (std_stream_of(guest_file) != NOT_STD) {
    ctx.ret(0);
    return;
  }
  int result = fclose(file_host(guest_file));
  gfree(guest_file);
  ctx.ret(result);
}

static void import_fwrite(Ctx& ctx, const Stub&) {
  u32 guest_file = ctx.a(3);
  if (std_stream_of(guest_file) > STD_IN) {
    log_out(std_stream_of(guest_file) == STD_ERR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "a32.stdio",
            std::string((char*)ctx.p(0), ctx.a(1) * ctx.a(2)));
    ctx.ret(ctx.a(2));
    return;
  }
  ctx.ret((u32)fwrite(ctx.p(0), ctx.a(1), ctx.a(2), file_host(guest_file)));
}

static void import_fputs(Ctx& ctx, const Stub&) {
  u32 guest_file = ctx.a(1);
  if (std_stream_of(guest_file) > STD_IN) {
    log_out(ANDROID_LOG_INFO, "a32.stdio", (char*)ctx.p(0));
    ctx.ret(1);
    return;
  }
  ctx.ret(fputs((const char*)ctx.p(0), file_host(guest_file)));
}

static void import_fputc(Ctx& ctx, const Stub&) {
  u32 guest_file = ctx.a(1);
  if (std_stream_of(guest_file) > STD_IN) {
    ctx.ret(ctx.a(0));
    return;
  }
  ctx.ret(fputc((int)ctx.a(0), file_host(guest_file)));
}

static void import_puts(Ctx& ctx, const Stub&) {
  log_out(ANDROID_LOG_INFO, "a32.stdio", (char*)ctx.p(0));
  ctx.ret(1);
}

static void import_putchar(Ctx& ctx, const Stub&) {
  char text[2] = {(char)ctx.a(0), 0};
  log_out(ANDROID_LOG_INFO, "a32.stdio", text);
  ctx.ret(ctx.a(0) & 0xff);
}

// Writes formatted text to a guest FILE: stdout / stderr go to the log, other streams to the host file.
static void write_formatted(Ctx& ctx, u32 guest_file, const std::string& text) {
  if (std_stream_of(guest_file) > STD_IN)
    log_out(ANDROID_LOG_INFO, "a32.stdio", text);
  else
    fwrite(text.data(), 1, text.size(), file_host(guest_file));
  ctx.ret((u32)text.size());
}

static void import_fprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 2, 0, false};
  write_formatted(ctx, ctx.a(0), format_guest((const char*)ctx.p(1), args));
}

static void import_vfprintf(Ctx& ctx, const Stub&) {  // (FILE*, fmt, va_list)
  Args args{&ctx, 0, ctx.a(2), true};
  write_formatted(ctx, ctx.a(0), format_guest((const char*)ctx.p(1), args));
}

static void import_android_log_print(Ctx& ctx, const Stub&) {
  Args args{&ctx, 3, 0, false};
  std::string text = format_guest((const char*)ctx.p(2), args);
  log_out((int)ctx.a(0), (const char*)ctx.p(1), text);
  ctx.ret(1);
}

static void import_vasprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 0, ctx.a(2), true};
  std::string text = format_guest((const char*)ctx.p(1), args);
  u32 buffer = gmalloc(text.size() + 1);
  memcpy(H(buffer), text.c_str(), text.size() + 1);
  *(u32*)ctx.p(0) = buffer;
  ctx.ret((u32)text.size());
}

static void put_str(Ctx& ctx, u32 dst, u32 cap,
                    const std::string& text) {  // snprintf semantics; cap 0xffffffff = unbounded
  if (cap) {
    size_t count = cap == 0xffffffffu ? text.size() : std::min<size_t>(text.size(), cap - 1);
    memcpy(H(dst), text.data(), count);
    ((char*)H(dst))[count] = 0;
  }
  ctx.ret((u32)text.size());
}

static void import_sprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 2, 0, false};
  put_str(ctx, ctx.a(0), 0xffffffffu, format_guest((const char*)ctx.p(1), args));
}

static void import_snprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 3, 0, false};
  put_str(ctx, ctx.a(0), ctx.a(1), format_guest((const char*)ctx.p(2), args));
}

static void import_vsprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 0, ctx.a(2), true};
  put_str(ctx, ctx.a(0), 0xffffffffu, format_guest((const char*)ctx.p(1), args));
}

static void import_vsnprintf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 0, ctx.a(3), true};
  put_str(ctx, ctx.a(0), ctx.a(1), format_guest((const char*)ctx.p(2), args));
}

static void import_android_log_vprint(Ctx& ctx, const Stub&) {  // (prio, tag, fmt, va_list)
  Args args{&ctx, 0, ctx.a(3), true};
  log_out((int)ctx.a(0), (const char*)ctx.p(1), format_guest((const char*)ctx.p(2), args));
  ctx.ret(1);
}

static void import_noop(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void import_printf(Ctx& ctx, const Stub&) {
  Args args{&ctx, 1, 0, false};
  std::string text = format_guest((const char*)ctx.p(0), args);
  log_out(ANDROID_LOG_INFO, "a32.stdio", text);
  ctx.ret((u32)text.size());
}

static void import_vswprintf(Ctx& ctx, const Stub&) {  // (wchar_t* buf, size_t n, const wchar_t* fmt, va_list)
  std::string format;
  for (const u32* w = (const u32*)ctx.p(2); *w; w++)
    format += (char)*w;  // format strings are ASCII
  Args args{&ctx, 0, ctx.a(3), true};
  std::string text = format_guest(format.c_str(), args);
  u32 cap = ctx.a(1), count = 0;
  u32* dest = (u32*)ctx.p(0);
  for (size_t i = 0; i < text.size() && count + 1 < cap;) {  // UTF-8 -> wchar_t
    uint8_t lead_byte = text[i];
    u32 code_point;
    int length = lead_byte < 0x80 ? 1 : lead_byte < 0xE0 ? 2 : lead_byte < 0xF0 ? 3 : 4;
    code_point = length == 1 ? lead_byte : lead_byte & (0xFF >> (length + 1));
    for (int k = 1; k < length && i + k < text.size(); k++)
      code_point = code_point << 6 | (text[i + k] & 0x3F);
    dest[count++] = code_point;
    i += length;
  }
  if (cap)
    dest[count] = 0;
  ctx.ret(count);
}

static void import_fstat(Ctx& ctx, const Stub&) {
  struct stat info;
  int result = fstat((int)ctx.a(0), &info);
  set_errno();
  if (result == 0)
    stat_to_guest(info, ctx.a(1));
  ctx.ret(result);
}

static void import_fdopen(Ctx& ctx, const Stub&) {
  FILE* file = fdopen((int)ctx.a(0), (const char*)ctx.p(1));
  set_errno();
  ctx.ret(file_new(file));
}

static void import_ioctl(Ctx& ctx, const Stub&) {  // third argument is an int or a pointer; guest pointers are >= 64K
  u32 arg = ctx.a(2);
  int result = ioctl((int)ctx.a(0), (unsigned long)ctx.a(1), arg < 0x10000 ? (void*)(uintptr_t)arg : H(arg));
  set_errno();
  ctx.ret(result);
}

static void import_writev(Ctx& ctx, const Stub&) {
  int count = (int)ctx.a(2);
  std::vector<struct iovec> vectors(count > 0 ? count : 0);
  const u32* guest_iov = (const u32*)ctx.p(1);
  for (int i = 0; i < count; i++) {
    vectors[i].iov_base = H(guest_iov[2 * i]);
    vectors[i].iov_len = guest_iov[2 * i + 1];
  }
  ssize_t result = writev((int)ctx.a(0), vectors.data(), count);
  set_errno();
  ctx.ret((u32)result);
}

static void import_android_log_assert(Ctx& ctx, const Stub&) {  // (cond, tag, fmt, ...)
  Args args{&ctx, 3, 0, false};
  std::string text =
      ctx.a(2) ? format_guest((const char*)ctx.p(2), args) : (ctx.a(0) ? (const char*)ctx.p(0) : "assert");
  __android_log_write(ANDROID_LOG_FATAL, ctx.a(1) ? (const char*)ctx.p(1) : "a32", text.c_str());
  abort();
}

static void import_sscanf(Ctx& ctx, const Stub&) {
  scan_guest(ctx, 0, false, false);
}

static void import_swscanf(Ctx& ctx, const Stub&) {
  scan_guest(ctx, 0, true, false);
}

static void import_fscanf(Ctx& ctx, const Stub&) {
  scan_guest(ctx, 0, false, true);
}

static void import_vsscanf(Ctx& ctx, const Stub&) {
  u32 va_list_addr = ctx.a(2);
  void* pointers[8];
  for (int i = 0; i < 8; i++)
    pointers[i] = H(((u32*)H(va_list_addr))[i]);
  ctx.ret(sscanf((const char*)ctx.p(0), (const char*)ctx.p(1), pointers[0], pointers[1], pointers[2], pointers[3],
                 pointers[4], pointers[5], pointers[6], pointers[7]));
}

static void import_opendir(Ctx& ctx, const Stub&) {
  DIR* dir = opendir(guest_host_path((const char*)ctx.p(0)));
  set_errno();
  if (!dir) {
    ctx.ret(0);
    return;
  }
  u32 guest_dir = gmalloc(8 + 280);
  *(DIR**)H(guest_dir) = dir;
  ctx.ret(guest_dir);
}

static void import_closedir(Ctx& ctx, const Stub&) {
  u32 guest_dir = ctx.a(0);
  int result = closedir(*(DIR**)H(guest_dir));
  gfree(guest_dir);
  ctx.ret(result);
}

static void import_readdir(Ctx& ctx, const Stub&) {
  u32 guest_dir = ctx.a(0);
  dirent* entry = readdir(*(DIR**)H(guest_dir));
  if (!entry) {
    ctx.ret(0);
    return;
  }
  memcpy(H(guest_dir + 8), entry, 19 + strlen(entry->d_name) + 1);
  ctx.ret(guest_dir + 8);
}

static void import_readdir_r(Ctx& ctx, const Stub&) {
  dirent* entry = readdir(*(DIR**)ctx.p(0));
  if (!entry) {
    *(u32*)ctx.p(2) = 0;
    ctx.ret(0);
    return;
  }
  memcpy(ctx.p(1), entry, 19 + strlen(entry->d_name) + 1);
  *(u32*)ctx.p(2) = ctx.a(1);
  ctx.ret(0);
}

// gethostbyname / inet_ntoa return pointers to static storage; the guest needs 32-bit layouts in guest memory.
// hostent (32-bit): { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; }
static u32 g_hostent;  // 20-byte hostent, then name[256], aliases[1], addr_list[2], addr[4]
static std::mutex g_hostent_mutex;

static void import_gethostbyname(Ctx& ctx, const Stub&) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo((const char*)ctx.p(0), nullptr, &hints, &result) != 0 || !result) {
    LOG("gethostbyname(%s) failed", (const char*)ctx.p(0));
    ctx.ret(0);  // h_errno is not modelled
    return;
  }
  std::lock_guard<std::mutex> lock(g_hostent_mutex);
  if (!g_hostent)
    g_hostent = gdata_alloc(20 + 256 + 4 + 8 + 4, 8);
  u32 name = g_hostent + 20, aliases = name + 256, list = aliases + 4, addr = list + 8;
  strncpy((char*)H(name), (const char*)ctx.p(0), 255);
  ((char*)H(name))[255] = 0;
  memcpy(H(addr), &((sockaddr_in*)result->ai_addr)->sin_addr, 4);
  freeaddrinfo(result);
  u32* hostent = (u32*)H(g_hostent);
  hostent[0] = name;
  hostent[1] = aliases;
  hostent[2] = AF_INET;
  hostent[3] = 4;
  hostent[4] = list;
  *(u32*)H(aliases) = 0;
  ((u32*)H(list))[0] = addr;
  ((u32*)H(list))[1] = 0;
  ctx.ret(g_hostent);
}

// getaddrinfo / freeaddrinfo / gai_strerror, IPv4 only. A guest addrinfo is bionic's 32-byte layout
// { flags, family, socktype, protocol, addrlen, canonname*, addr*, next* } followed by its 16-byte sockaddr_in;
// freed blocks are kept for the next call.
enum : u32 { GUEST_ADDRINFO_SIZE = 32 + 16, EAI_NONAME_CODE = 8 };
static std::mutex g_addrinfo_mutex;
static std::vector<u32> g_addrinfo_free;

static void import_getaddrinfo(Ctx& ctx, const Stub&) {
  const char* node = (const char*)ctx.p(0);
  const char* service = ctx.a(1) ? (const char*)ctx.p(1) : nullptr;
  u32* out = (u32*)ctx.p(3);
  if (!node || !out) {
    ctx.ret(EAI_NONAME_CODE);
    return;
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(node, nullptr, &hints, &result) != 0 || !result) {
    LOG("getaddrinfo(%s) failed", node);
    *out = 0;
    ctx.ret(EAI_NONAME_CODE);
    return;
  }
  u32 block;
  {
    std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
    if (g_addrinfo_free.empty()) {
      block = gdata_alloc(GUEST_ADDRINFO_SIZE, 8);
    } else {
      block = g_addrinfo_free.back();
      g_addrinfo_free.pop_back();
    }
  }
  sockaddr_in address = *(sockaddr_in*)result->ai_addr;
  address.sin_port = htons((unsigned short)(service ? atoi(service) : 0));
  freeaddrinfo(result);
  u32* info = (u32*)H(block);
  info[0] = 0;
  info[1] = AF_INET;
  info[2] = SOCK_STREAM;
  info[3] = IPPROTO_TCP;
  info[4] = sizeof(sockaddr_in);
  info[5] = 0;
  info[6] = block + 32;
  info[7] = 0;
  memcpy(H(block + 32), &address, sizeof address);
  *out = block;
  ctx.ret(0);
}

static void import_freeaddrinfo(Ctx& ctx, const Stub&) {
  if (!ctx.a(0))
    return;
  std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
  g_addrinfo_free.push_back(ctx.a(0));
}

static void import_gai_strerror(Ctx& ctx, const Stub&) {
  static u32 text = 0;
  if (!text) {
    text = gdata_alloc(32, 4);
    strcpy((char*)H(text), "name resolution failed");
  }
  ctx.ret(text);
}

static void import_inet_ntoa(Ctx& ctx, const Stub&) {  // struct in_addr is passed by value: one u32
  static u32 buffer = gdata_alloc(16, 4);
  in_addr address;
  address.s_addr = ctx.a(0);
  strncpy((char*)H(buffer), inet_ntoa(address), 15);
  ((char*)H(buffer))[15] = 0;
  ctx.ret(buffer);
}

void register_stdio_imports() {
  g_std_streams = gdata_alloc(GUEST_STD_STREAMS * GUEST_FILE_SIZE, 8);
  memset(H(g_std_streams), 0, GUEST_STD_STREAMS * GUEST_FILE_SIZE);
  sym_add("__sF", g_std_streams);

  // FILE streams
  hle("fopen", import_fopen);
  hle("fclose", import_fclose);
  hle("fwrite", import_fwrite);
  hle("fputs", import_fputs);
  hle("fputc", import_fputc);
  hle("puts", import_puts);
  hle("fprintf", import_fprintf);
  hle("vfprintf", import_vfprintf);
  hle("putchar", import_putchar);
  hle("fsetpos", import_fsetpos);
  hle("fdopen", import_fdopen);
  hle("setvbuf", [](Ctx& ctx, const Stub&) { ctx.ret(0); });  // buffering stays with the host FILE
  gen("fread", "puuF>u");
  gen("fflush", "F>i");
  gen("fseek", "Fii>i");
  gen("ftell", "F>i");
  gen("fgetc", "F>i");
  gen("getc", "F>i");
  gen("fgets", "piF>P");
  gen("ungetc", "iF>i");
  gen("fwide", "Fi>i");
  gen("fputwc", "uF>u");
  gen("getwc", "F>u");
  gen("putwc", "uF>u");
  gen("ungetwc", "uF>u");
  gen("feof", "F>i");
  gen("ferror", "F>i");
  gen("putc", "iF>i");

  // formatted I/O
  hle("sscanf", import_sscanf);
  hle("swscanf", import_swscanf);
  hle("fscanf", import_fscanf);
  hle("vsscanf", import_vsscanf);
  hle("vasprintf", import_vasprintf);
  hle("sprintf", import_sprintf);
  hle("snprintf", import_snprintf);
  hle("vsprintf", import_vsprintf);
  hle("vsnprintf", import_vsnprintf);
  hle("printf", import_printf);
  hle("__android_log_vprint", import_android_log_vprint);
  hle("tzset", import_noop);
  hle("vswprintf", import_vswprintf);

  // logging
  hle("__android_log_print", import_android_log_print);
  hle("__android_log_assert", import_android_log_assert);
#if defined(A32_HOST_RUNNER)  // the host has no liblog: use the shim in tools/jiagu_run/shim
  hle("__android_log_write", [](Ctx& ctx, const Stub&) {
    ctx.ret(__android_log_write(ctx.a(0), (const char*)ctx.p(1), (const char*)ctx.p(2)));
  });
#elif defined(A32_LOG)
  gen("__android_log_write", "ipp>i", "liblog.so");
#else
  hle("__android_log_write", [](Ctx& ctx, const Stub&) { ctx.ret(1); });
#endif

  // files and directories
  gen("ftruncate", "ii>i");
  gen("rename", "ss>i");
  hle("access", import_access);
  gen("mkdir", "su>i");
  gen("unlink", "s>i");
  gen("rmdir", "s>i");
  gen("chdir", "p>i");
  hle("chmod", [](Ctx& ctx, const Stub&) {  // shared storage ignores modes
    const char* path = (const char*)ctx.p(0);
    const char* host = guest_host_path(path);
    int result = host != path ? 0 : chmod(host, ctx.a(1));
    set_errno();
    ctx.ret(result);
  });
  gen("getcwd", "pu>P");
  gen("readlink", "ppu>i");
  gen("fsync", "i>i");
  gen("lseek", "iii>i");
  gen("close", "i>i");
  gen("read", "ipu>i");
  gen("write", "ipu>i");
  gen("poll", "pui>i");
  hle("open", import_open);
  hle("stat", import_stat);
  hle("fstat", import_fstat);
  hle("statfs", import_statfs);
  hle("utime", import_utime);
  hle("ioctl", import_ioctl);
  hle("writev", import_writev);
  hle("select", import_select);
  hle("opendir", import_opendir);
  hle("closedir", import_closedir);
  hle("readdir", import_readdir);
  hle("readdir_r", import_readdir_r);

  // sockets
  gen("socket", "iii>i");
  gen("setsockopt", "iiipu>i");
  hle("connect", [](Ctx& ctx, const Stub&) {
    const sockaddr* address = (const sockaddr*)ctx.p(1);
    if (ctx.a(2) >= sizeof(sockaddr_in)) {
      LogRelay relay = log_relay_connect((int)ctx.a(0), *(const sockaddr_in*)address);
      if (relay == LogRelay::Relayed) {
        ctx.ret(0);
        return;
      }
      if (relay == LogRelay::Refused) {
        errno = ECONNREFUSED;
        set_errno();
        ctx.ret((u32)-1);
        return;
      }
    }
    int result = connect((int)ctx.a(0), address, ctx.a(2));
    set_errno();
    ctx.ret((u32)result);
  });
  gen("bind", "ipu>i");
  gen("listen", "ii>i");
  gen("accept", "ipp>i");
  // send/recv: A32_NETLOG=1 prints the first bytes of each transfer (packers talk to their own servers)
  hle("send", [](Ctx& ctx, const Stub&) {
    ssize_t n = send((int)ctx.a(0), ctx.p(1), ctx.a(2), (int)ctx.a(3));
    if (getenv("A32_NETLOG"))
      LOG("send(%d, %u bytes) = %zd: %.*s", (int)ctx.a(0), ctx.a(2), n, (int)std::min<u32>(ctx.a(2), 400), (const char*)ctx.p(1));
    ctx.ret((u32)n);
  });
  hle("recv", [](Ctx& ctx, const Stub&) {
    ssize_t n = recv((int)ctx.a(0), ctx.p(1), ctx.a(2), (int)ctx.a(3));
    if (getenv("A32_NETLOG"))
      LOG("recv(%d) = %zd: %.*s", (int)ctx.a(0), n, (int)std::max<ssize_t>(0, std::min<ssize_t>(n, 400)), (const char*)ctx.p(1));
    ctx.ret((u32)n);
  });
  gen("sendto", "ipuipu>i");
  gen("recvfrom", "ipuipp>i");
  gen("shutdown", "ii>i");
  gen("getsockname", "ipp>i");
  gen("gethostname", "pu>i");
  gen("inet_addr", "p>u");
  hle("gethostbyname", import_gethostbyname);
  hle("getaddrinfo", import_getaddrinfo);
  hle("freeaddrinfo", import_freeaddrinfo);
  hle("gai_strerror", import_gai_strerror);
  hle("inet_ntoa", import_inet_ntoa);
}
