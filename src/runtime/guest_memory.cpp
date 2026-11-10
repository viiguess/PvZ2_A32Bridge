// Guest memory: the host reservation, a permanent data arena, and the guest heap.
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "runtime/runtime.h"

uint8_t* G;

// ---------------------------------------------------------------- guest data + heap
static std::mutex g_heap_mu;
static u32 g_data_next = DATA_BASE;
static u32 g_heap_next = HEAP_BASE;
static std::unordered_map<u32, std::vector<u32>> g_free;  // usable size -> blocks
static std::unordered_map<std::string, u32> g_strs;

// Blocks over 1 MiB are page granular and live in an address-ordered free map that coalesces neighbours, and the pages
// of a freed block go back to the host. The exact-size lists above never merge, so a game that copies many differently
// sized files through big buffers (the first start of PvZ2C copies about 1 GB of .rsb files) used up the address space.
constexpr u32 LARGE_MIN = 1u << 20;
static std::map<u32, u32> g_large_free;  // block start -> block size (both page multiples)

static u32 large_alloc(u32 size) {  // g_heap_mu held; returns the user pointer or 0
  u64 total = ((u64)size + 8 + 4095) & ~4095ull;
  if (total > 0xffff0000ull)
    return 0;
  u32 block = 0;
  auto best = g_large_free.end();
  for (auto it = g_large_free.begin(); it != g_large_free.end(); ++it)
    if (it->second >= total && (best == g_large_free.end() || it->second < best->second))
      best = it;
  if (best != g_large_free.end()) {
    block = best->first;
    u32 rest = best->second - (u32)total;
    g_large_free.erase(best);
    if (rest)
      g_large_free[block + (u32)total] = rest;
  } else {
    u64 start = ((u64)g_heap_next + 4095) & ~4095ull;
    if (start + total > HEAP_END)
      return 0;
    block = (u32)start;
    g_heap_next = (u32)(start + total);
  }
  u32 ptr = block + 8;
  ((u32*)(G + ptr))[-2] = (u32)total - 8;
  ((u32*)(G + ptr))[-1] = ptr;
  return ptr;
}

static void large_free(u32 ptr) {  // g_heap_mu held
  u32 block = ptr - 8, total = ((u32*)(G + ptr))[-2] + 8;
  madvise(G + block, total, MADV_DONTNEED);
  auto next = g_large_free.lower_bound(block);
  if (next != g_large_free.end() && block + total == next->first) {
    total += next->second;
    next = g_large_free.erase(next);
  }
  if (next != g_large_free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == block) {
      block = prev->first;
      total += prev->second;
      g_large_free.erase(prev);
    }
  }
  g_large_free[block] = total;
}

u32 gdata_alloc(u32 size, u32 alignment) {
  std::lock_guard<std::mutex> lock(g_heap_mu);
  g_data_next = (g_data_next + alignment - 1) & ~(alignment - 1);
  u32 addr = g_data_next;
  g_data_next += size;
  if (g_data_next > DATA_BASE + DATA_SIZE) {
    LOG("guest data exhausted");
    abort();
  }
  return addr;
}

u32 gstr(const char* text) {
  {
    std::lock_guard<std::mutex> lock(g_heap_mu);
    auto it = g_strs.find(text);
    if (it != g_strs.end())
      return it->second;
  }
  u32 addr = gdata_alloc(strlen(text) + 1, 1);
  memcpy(G + addr, text, strlen(text) + 1);
  std::lock_guard<std::mutex> lock(g_heap_mu);
  g_strs[text] = addr;
  return addr;
}

static u32 round_size(u32 size) {
  if (size < 16)
    size = 16;
  if (size <= 1024)
    return (size + 7) & ~7u;
  if (size <= 65536)
    return (size + 255) & ~255u;
  if (size <= (1u << 20))
    return (size + 4095) & ~4095u;
  // Large blocks come in 8 sizes per power of two, so that the many differently sized buffers a game allocates (and
  // frees) can be reused by each other; the free lists are exact-size and the heap never coalesces.
  u32 top = 31 - __builtin_clz(size - 1);  // the power of two below `size`
  u32 step = (1u << top) / 8;
  u64 rounded = ((u64)size + step - 1) / step * step;
  return rounded > 0xfffff000u ? (size + 4095) & ~4095u : (u32)rounded;
}

// block layout: [usable size u32][base u32][ user data ... ]; user ptr is 8-aligned
u32 gmalloc(u32 size) {
  u32 rounded = round_size(size);
  std::lock_guard<std::mutex> lock(g_heap_mu);
  if (size > LARGE_MIN) {
    u32 big = large_alloc(size);
    if (!big)
      LOG("guest heap exhausted: req=%u next=%08x", size, g_heap_next);
    return big;
  }
  auto& blocks = g_free[rounded];
  u32 ptr;
  if (!blocks.empty()) {
    ptr = blocks.back();
    blocks.pop_back();
  } else {
    if (g_heap_next + rounded + 8 > HEAP_END || g_heap_next + rounded + 8 < g_heap_next) {
      // Out of fresh address space: take the smallest free block that is big enough (it keeps its own size)
      u32 best = 0;
      for (auto& entry : g_free)
        if (entry.first >= rounded && !entry.second.empty() && (!best || entry.first < best))
          best = entry.first;
      if (!best) {
        LOG("guest heap exhausted: req=%u next=%08x", size, g_heap_next);
        return 0;
      }
      ptr = g_free[best].back();
      g_free[best].pop_back();
      rounded = best;
    } else {
      ptr = g_heap_next + 8;
      g_heap_next += rounded + 8;
    }
  }
  ((u32*)(G + ptr))[-2] = rounded;
  ((u32*)(G + ptr))[-1] = ptr;
  return ptr;
}

void gfree(u32 ptr) {
  if (!ptr)
    return;
  u32* header = (u32*)(G + ptr) - 2;
  u32 base = header[1];
  if (base != ptr) {
    gfree(base);
    return;
  }  // memalign'd
  std::lock_guard<std::mutex> lock(g_heap_mu);
  if (header[0] > LARGE_MIN) {
    large_free(ptr);
    return;
  }
  g_free[header[0]].push_back(ptr);
}

u32 gmemalign(u32 alignment, u32 size) {
  if (alignment <= 8)
    return gmalloc(size);
  u32 base = gmalloc(size + alignment + 8);
  if (!base)
    return 0;
  u32 ptr = (base + 8 + alignment - 1) & ~(alignment - 1);
  ((u32*)(G + ptr))[-2] = 0;
  ((u32*)(G + ptr))[-1] = base;
  return ptr;
}
