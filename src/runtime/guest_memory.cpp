// Guest memory: the host reservation, a permanent data arena, and the guest heap.
#include <stdlib.h>
#include <string.h>

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
