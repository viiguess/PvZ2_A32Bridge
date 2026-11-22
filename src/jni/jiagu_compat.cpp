// Old 360 Jiagu packers (Lenovo PvZ2C 2.2.2 and earlier, version codes <= 865) inline-hook ART: they read
// art::OatHeader::kOatVersion and parse /system/lib/libart.so, both from disk and from memory, for a list of ART symbols,
// and they refuse to start (JNI_OnLoad returns JNI_ERR) without them. tools/jiagu_run does the same set-up for unpacking.
//
// port_apk.py ships lib/<abi>/liba32jg.so, a text file, only for those builds: the first line is "art", the other lines
// are the ART symbol names to export. Without the file nothing here runs (newer packers change behaviour when they find
// a libart image).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <zlib.h>

#include <string>
#include <utility>
#include <vector>

#include "jni/bridge.h"
#include "libc/system_imports.h"
#include "runtime/guest_memory.h"
#include "runtime/stubs.h"

namespace {

struct Out {
  std::vector<uint8_t> b;
  void u16(uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
  void u32(uint32_t v) { u16(v & 0xffff); u16(v >> 16); }
  void raw(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

uint32_t elf_hash(const std::string& name) {
  uint32_t h = 0;
  for (unsigned char c : name) {
    h = (h << 4) + c;
    uint32_t g = h & 0xf0000000;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

// A minimal ARM32 ELF shared object that exports the given symbols (port of tools/jiagu_run/fake_libart.py).
std::vector<uint8_t> build_fake_elf(const std::vector<std::pair<std::string, uint32_t>>& syms) {
  std::string strtab(1, '\0');
  std::vector<uint32_t> name_off;
  for (auto& s : syms) {
    name_off.push_back((uint32_t)strtab.size());
    strtab += s.first;
    strtab.push_back('\0');
  }
  uint32_t nsym = (uint32_t)syms.size() + 1;
  Out dynsym;
  for (int i = 0; i < 4; i++)
    dynsym.u32(0);
  for (size_t i = 0; i < syms.size(); i++) {
    dynsym.u32(name_off[i]);
    dynsym.u32(syms[i].second);
    dynsym.u32(4);
    dynsym.b.push_back(0x12);
    dynsym.b.push_back(0);
    dynsym.u16(1);
  }
  const uint32_t nb = 67;
  std::vector<uint32_t> buckets(nb, 0), chains(nsym, 0);
  for (size_t i = 0; i < syms.size(); i++) {
    uint32_t bucket = elf_hash(syms[i].first) % nb;
    chains[i + 1] = buckets[bucket];
    buckets[bucket] = (uint32_t)i + 1;
  }
  Out hash;
  hash.u32(nb);
  hash.u32(nsym);
  for (uint32_t v : buckets) hash.u32(v);
  for (uint32_t v : chains) hash.u32(v);
  std::string shstr("\0.hash\0.dynsym\0.dynstr\0.symtab\0.strtab\0.shstrtab\0.dynamic\0", 58);
  auto shname = [&](const char* n) { return (uint32_t)(shstr.find(std::string("\0", 1) + n + std::string("\0", 1)) + 1); };

  std::vector<uint8_t> body;
  uint32_t off = 52 + 32 * 2;
  auto place = [&](const std::vector<uint8_t>& data, uint32_t align) {
    uint32_t pad = (align - off % align) % align;
    body.insert(body.end(), pad, 0);
    off += pad;
    uint32_t start = off;
    body.insert(body.end(), data.begin(), data.end());
    off += (uint32_t)data.size();
    return start;
  };
  std::vector<uint8_t> strtab_v(strtab.begin(), strtab.end()), shstr_v(shstr.begin(), shstr.end());
  uint32_t o_hash = place(hash.b, 4), o_dynsym = place(dynsym.b, 4), o_dynstr = place(strtab_v, 4);
  uint32_t o_symtab = place(dynsym.b, 4), o_strtab = place(strtab_v, 4);
  Out dyn;
  uint32_t entries[6][2] = {{4, o_hash}, {5, o_dynstr}, {6, o_dynsym}, {10, (uint32_t)strtab.size()}, {11, 16}, {0, 0}};
  for (auto& e : entries) { dyn.u32(e[0]); dyn.u32(e[1]); }
  uint32_t o_dyn = place(dyn.b, 4), o_shstr = place(shstr_v, 1);
  uint32_t end_load = off, shoff = off + (4 - off % 4) % 4;
  body.insert(body.end(), shoff - off, 0);

  Out shs;
  auto sh = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t addr, uint32_t offs, uint32_t size, uint32_t link,
                uint32_t info, uint32_t align, uint32_t ent) {
    for (uint32_t v : {name, type, flags, addr, offs, size, link, info, align, ent})
      shs.u32(v);
  };
  sh(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  sh(shname(".hash"), 5, 2, o_hash, o_hash, (uint32_t)hash.b.size(), 2, 0, 4, 4);
  sh(shname(".dynsym"), 11, 2, o_dynsym, o_dynsym, (uint32_t)dynsym.b.size(), 3, 1, 4, 16);
  sh(shname(".dynstr"), 3, 2, o_dynstr, o_dynstr, (uint32_t)strtab.size(), 0, 0, 1, 0);
  sh(shname(".symtab"), 2, 0, 0, o_symtab, (uint32_t)dynsym.b.size(), 5, 1, 4, 16);
  sh(shname(".strtab"), 3, 0, 0, o_strtab, (uint32_t)strtab.size(), 0, 0, 1, 0);
  sh(shname(".dynamic"), 6, 3, o_dyn, o_dyn, (uint32_t)dyn.b.size(), 3, 0, 4, 8);
  sh(shname(".shstrtab"), 3, 0, 0, o_shstr, (uint32_t)shstr.size(), 0, 0, 1, 0);

  Out file;
  file.raw({0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  file.u16(3); file.u16(40); file.u32(1); file.u32(0); file.u32(52); file.u32(shoff); file.u32(0x05000000);
  file.u16(52); file.u16(32); file.u16(2); file.u16(40); file.u16(8); file.u16(7);
  for (uint32_t v : {1u, 0u, 0u, 0u, end_load, end_load, 5u, 0x1000u}) file.u32(v);
  for (uint32_t v : {2u, o_dyn, o_dyn, o_dyn, (uint32_t)dyn.b.size(), (uint32_t)dyn.b.size(), 6u, 4u}) file.u32(v);
  file.raw(body);
  file.raw(shs.b);
  return file.b;
}

std::string find_shipped(const char* file) {
  std::string dir = native_library_dir();
  if (dir.empty())
    return "";
  std::string parent = dir.substr(0, dir.rfind('/'));
  for (const char* abi : {"x86_64", "arm64-v8a", "arm64", "x86"}) {
    std::string candidate = parent + "/" + abi + "/" + file;
    if (access(candidate.c_str(), R_OK) == 0)
      return candidate;
  }
  return "";
}

std::string package_name() {
  FILE* f = fopen("/proc/self/cmdline", "r");
  char name[256] = {0};
  if (f) {
    size_t n = fread(name, 1, sizeof name - 1, f);
    name[n] = 0;
    fclose(f);
  }
  return name;
}

// Maps a fake ELF image into the guest, lists it in the fake /proc/self/maps and serves it for `guest_path`.
void install_image(const std::vector<std::string>& names, const char* guest_path, const char* host_name, int spare_start) {
  u32 base = gdata_alloc(0x9000 + 0x1000, 0x1000);
  base = (base + 0xfff) & ~0xfffu;
  std::vector<std::pair<std::string, uint32_t>> syms;
  int listed = 0;
  for (const std::string& name : names) {
    // A symbol the bridge does not implement still has to exist: the packer inline-hooks it. Point it at a spare spot
    // of the image (below the 0x7000 bytes copied in, so the hook can write there).
    u32 stub = stub_find(name);
    syms.emplace_back(name, stub ? stub - base : (uint32_t)(spare_start + 0x20 * listed));
    listed++;
  }
  std::vector<uint8_t> image = build_fake_elf(syms);
  size_t copy = image.size() < 0x7000 ? image.size() : 0x7000;
  memcpy(H(base), image.data(), copy);
  fake_proc_maps_add_region(base, 0x8000, guest_path);
  std::string host = "/data/data/" + package_name() + "/" + host_name;
  if (FILE* f = fopen(host.c_str(), "wb")) {
    fwrite(image.data(), 1, image.size(), f);
    fclose(f);
    std::string old = getenv("A32_PATH_MAP") ? std::string(getenv("A32_PATH_MAP")) + ";" : std::string();
    setenv("A32_PATH_MAP", (old + guest_path + "=" + host).c_str(), 1);
  }
  LOG("jiagu compat: fake %s (%d symbols) at %08x", guest_path, listed, base);
}

// The shell classes.dex of the APK, inflated into the guest and listed in the fake /proc/self/maps under the name the
// runtime gives its mapping. Old packers find it there and read their payload from it.
void map_shell_dex() {
  std::string libdir = native_library_dir();
  size_t lib = libdir.rfind("/lib/");
  if (lib == std::string::npos)
    return;
  FILE* f = fopen((libdir.substr(0, lib) + "/base.apk").c_str(), "rb");
  if (!f)
    return;
  auto u16 = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; };
  auto u32le = [&](const uint8_t* p) { return u16(p) | u16(p + 2) << 16; };
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long tail = size < 70000 ? size : 70000;
  std::vector<uint8_t> end(tail);
  fseek(f, size - tail, SEEK_SET);
  if (fread(end.data(), 1, tail, f) != (size_t)tail) {
    fclose(f);
    return;
  }
  long eocd = -1;
  for (long i = tail - 22; i >= 0; i--)
    if (u32le(&end[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    fclose(f);
    return;
  }
  uint32_t count = u16(&end[eocd + 10]), directory_size = u32le(&end[eocd + 12]), directory_at = u32le(&end[eocd + 16]);
  std::vector<uint8_t> directory(directory_size);
  fseek(f, directory_at, SEEK_SET);
  if (fread(directory.data(), 1, directory_size, f) != directory_size) {
    fclose(f);
    return;
  }
  uint32_t entry = 0, method = 0, packed = 0, unpacked = 0, local = 0;
  for (uint32_t at = 0, n = 0; n < count && at + 46 <= directory_size; n++) {
    const uint8_t* e = &directory[at];
    uint32_t name_length = u16(e + 28), extra_length = u16(e + 30), comment_length = u16(e + 32);
    if (name_length == 11 && !memcmp(e + 46, "classes.dex", 11)) {
      entry = 1;
      method = u16(e + 10);
      packed = u32le(e + 20);
      unpacked = u32le(e + 24);
      local = u32le(e + 42);
      break;
    }
    at += 46 + name_length + extra_length + comment_length;
  }
  if (!entry) {
    fclose(f);
    return;
  }
  uint8_t header[30];
  fseek(f, local, SEEK_SET);
  if (fread(header, 1, 30, f) != 30) {
    fclose(f);
    return;
  }
  fseek(f, local + 30 + u16(header + 26) + u16(header + 28), SEEK_SET);
  std::vector<uint8_t> data(packed);
  if (fread(data.data(), 1, packed, f) != packed) {
    fclose(f);
    return;
  }
  fclose(f);
  std::vector<uint8_t> dex(unpacked);
  if (method == 0) {
    dex = data;
  } else {
    z_stream stream = {};
    stream.next_in = data.data();
    stream.avail_in = packed;
    stream.next_out = dex.data();
    stream.avail_out = unpacked;
    if (inflateInit2(&stream, -15) != Z_OK)
      return;
    int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END)
      return;
  }
  u32 base = gdata_alloc((u32)dex.size() + 16, 0x1000);
  memcpy(H(base), dex.data(), dex.size());
  fake_proc_maps_add_region(base, (u32)dex.size(), "/data/dalvik-cache/arm/data@app@" + package_name() + "-1@base.apk@classes.dex");
  LOG("jiagu compat: shell classes.dex (%zu bytes) at %08x", dex.size(), base);
}

}  // namespace

void jiagu_compat_init() {
  std::string path = find_shipped("liba32jg.so");
  if (path.empty())
    return;
  std::vector<std::string> names;
  if (FILE* f = fopen(path.c_str(), "r")) {
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      if (first) {
        first = false;
        if (strcmp(line, "art"))
          break;
        continue;
      }
      if (line[0])
        names.push_back(line);
    }
    fclose(f);
  }
  if (names.empty())
    return;
  // art::OatHeader::kOatVersion: three digits for the device's Android version.
  char sdk_text[PROP_VALUE_MAX] = {0};
  __system_property_get("ro.build.version.sdk", sdk_text);
  int sdk = atoi(sdk_text);
  const char* oat = sdk >= 34 ? "230" : sdk >= 33 ? "223" : sdk >= 31 ? "195" : sdk >= 30 ? "183" : sdk >= 29 ? "170"
                  : sdk >= 28 ? "138" : sdk >= 27 ? "131" : sdk >= 26 ? "124" : sdk >= 24 ? "079" : sdk >= 23 ? "064"
                  : sdk >= 22 ? "045" : "039";
  u32 var = gdata_alloc(8, 4);
  memset(H(var), 0, 8);
  memcpy(H(var), oat, 3);
  sym_add("_ZN3art9OatHeader11kOatVersionE", var);
  install_image(names, "/system/lib/libart.so", "a32_libart.so", 0x4000);
  map_shell_dex();
  install_image({"rtld_db_dlactivity"}, "/system/bin/linker", "a32_linker.so", 0x1000);
}
