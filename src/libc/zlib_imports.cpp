// zlib: the 32-bit z_stream layout is converted to the host's on every call.
#include "libc/zlib_imports.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"

// ------------------------------------------------------------------ zlib (z_stream is 56 bytes in 32-bit)
static std::mutex g_z_mu;
static std::unordered_map<u32, z_stream*> g_z;

static z_stream* z_get(u32 guest_strm, bool create) {
  std::lock_guard<std::mutex> lock(g_z_mu);
  auto it = g_z.find(guest_strm);
  if (it != g_z.end() && !create)
    return it->second;
  z_stream* stream = new z_stream;
  memset(stream, 0, sizeof *stream);
  g_z[guest_strm] = stream;
  return stream;
}

static void z_in(u32 guest_strm, z_stream* stream) {
  u32* words = (u32*)H(guest_strm);
  stream->next_in = (Bytef*)H(words[0]);
  stream->avail_in = words[1];
  stream->total_in = words[2];
  stream->next_out = (Bytef*)H(words[3]);
  stream->avail_out = words[4];
  stream->total_out = words[5];
  stream->data_type = words[11];
  stream->adler = words[12];
}

static void z_out(u32 guest_strm, z_stream* stream) {
  u32* words = (u32*)H(guest_strm);
  words[0] = GA(stream->next_in);
  words[1] = stream->avail_in;
  words[2] = (u32)stream->total_in;
  words[3] = GA(stream->next_out);
  words[4] = stream->avail_out;
  words[5] = (u32)stream->total_out;
  words[6] = stream->msg ? gstr(stream->msg) : 0;
  words[11] = stream->data_type;
  words[12] = (u32)stream->adler;
}

template <int OP>
static void zlib_op(Ctx& ctx, const Stub&) {
  u32 guest_strm = ctx.a(0);
  z_stream* stream;
  int result;
  if (OP == 0) {  // inflateInit_(strm, version, size)
    stream = z_get(guest_strm, true);
    z_in(guest_strm, stream);  // fields set before init (legal with real zlib) must survive it
    result = inflateInit(stream);
    z_out(guest_strm, stream);
  } else if (OP == 8) {  // deflateInit_(strm, level, version, size)
    stream = z_get(guest_strm, true);
    z_in(guest_strm, stream);  // fields set before init (legal with real zlib) must survive it
    result = deflateInit(stream, (int)ctx.a(1));
    z_out(guest_strm, stream);
  } else if (OP == 9) {  // inflateInit2_(strm, windowBits, version, size)
    stream = z_get(guest_strm, true);
    z_in(guest_strm, stream);  // fields set before init (legal with real zlib) must survive it
    result = inflateInit2(stream, (int)ctx.a(1));
    z_out(guest_strm, stream);
  } else if (OP == 10) {  // deflateBound(strm, sourceLen)
    stream = z_get(guest_strm, false);
    ctx.ret(stream ? (u32)deflateBound(stream, ctx.a(1)) : ctx.a(1) + (ctx.a(1) >> 12) + (ctx.a(1) >> 14) + 13);
    return;
  } else if (OP == 1) {  // deflateInit2_(strm, level, method, wbits, memlevel, strategy, version, size)
    stream = z_get(guest_strm, true);
    z_in(guest_strm, stream);  // fields set before init (legal with real zlib) must survive it
    result = deflateInit2(stream, ctx.a(1), ctx.a(2), ctx.a(3), ctx.a(4), ctx.a(5));
    z_out(guest_strm, stream);
  } else {
    stream = z_get(guest_strm, false);
    if (!stream) {
      ctx.ret((u32)Z_STREAM_ERROR);
      return;
    }
    z_in(guest_strm, stream);
    switch (OP) {
      case 2:
        result = inflate(stream, ctx.a(1));
        break;
      case 3:
        result = inflateEnd(stream);
        break;
      case 4:
        result = inflateReset(stream);
        break;
      case 5:
        result = deflate(stream, ctx.a(1));
        break;
      case 6:
        result = deflateEnd(stream);
        break;
      default:
        result = deflateReset(stream);
        break;
    }
    z_out(guest_strm, stream);
    if (OP == 3 || OP == 6) {
      std::lock_guard<std::mutex> lock(g_z_mu);
      g_z.erase(guest_strm);
      delete stream;
    }
  }
  ctx.ret((u32)result);
}

static void import_compress(
    Ctx& ctx,
    const Stub&) {  // compress(dest, uLongf* destLen, src, srcLen): uLong is 32-bit in the guest
  uLongf size = *(u32*)ctx.p(1);
  int result = compress((Bytef*)ctx.p(0), &size, (const Bytef*)ctx.p(2), ctx.a(3));
  *(u32*)ctx.p(1) = (u32)size;
  ctx.ret((u32)result);
}

static void import_compress2(Ctx& ctx, const Stub&) {  // compress2(dest, uLongf* destLen, src, srcLen, level)
  uLongf size = *(u32*)ctx.p(1);
  int result = compress2((Bytef*)ctx.p(0), &size, (const Bytef*)ctx.p(2), ctx.a(3), (int)ctx.a(4));
  *(u32*)ctx.p(1) = (u32)size;
  ctx.ret((u32)result);
}

static void import_uncompress(Ctx& ctx, const Stub&) {
  uLongf size = *(u32*)ctx.p(1);
  int result = uncompress((Bytef*)ctx.p(0), &size, (const Bytef*)ctx.p(2), ctx.a(3));
  *(u32*)ctx.p(1) = (u32)size;
  ctx.ret((u32)result);
}

void register_zlib_imports() {
  hle("inflateInit_", zlib_op<0>);
  hle("deflateInit2_", zlib_op<1>);
  hle("inflateInit2_", zlib_op<9>);
  hle("deflateBound", zlib_op<10>);
  hle("inflate", zlib_op<2>);
  hle("inflateEnd", zlib_op<3>);
  hle("inflateReset", zlib_op<4>);
  hle("deflate", zlib_op<5>);
  hle("deflateEnd", zlib_op<6>);
  hle("deflateReset", zlib_op<7>);
  hle("deflateInit_", zlib_op<8>);
  hle("compress", import_compress);
  hle("compress2", import_compress2);
  hle("uncompress", import_uncompress);
  gen("crc32", "upu>u");
  gen("adler32", "upu>u");
}
