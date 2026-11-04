// "Free shop" patch for the Lenovo PvZ2C 32-bit builds (ARM and Thumb-2 code), as a pure function over the mapped image
// so it can be unit-tested off-device. Everything is located by instruction shape and string cross-references, never by
// offset, so the same code covers builds whose registers, optimisation level and class layout differ.
//
// What it changes in PurchaseBroker:
//   onPurchaseConfirm   instead of storing the order and entering NeedToSendPurchase (handing the order to the
//                       payment SDK), call PurchaseDriverPaymentComplete(this, _, sku): the normal validate/grant
//                       path runs without the SDK. The dialog that would wait for the SDK is skipped.
//   PurchaseSuccessed   the "purchase complete" dialog is skipped as well (it was never opened).
//   RequestPayment      (oldest builds only) the pay-check gate is forced open.
//
// Nothing is written unless every site was found exactly once.
//
// Class layout, as offsets from `this` (W = offset of m_watingTimes):
//   state S   (0x3c, 0x20 in 1.0.x)      requested sku S+4      [order number S+8 in 2.4.83+]
//   m_watingTimes W                      m_bShowDialog W+0x10   m_bNeedShowDialog W+0x11   m_playerInfo W+12
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace free_shop {

typedef uint32_t u32;

struct Result {
  bool applied = false;
  const char* error = "";
  u32 tail = 0, payment_complete = 0, service_check = 0, dialog_check = 0, success_dialog = 0;  // image offsets
};

namespace detail {

const u32 NOP = 0xE1A00000;  // mov r0,r0

inline int32_t branch_offset(u32 insn) {
  return ((int32_t)(insn << 8) >> 8) * 4;
}

inline bool is_call(u32 insn) {  // bl, or blx <imm> (switches to Thumb)
  return (insn >> 24) == 0xEB || (insn >> 25) == 0x7D;
}

inline bool is_blx(u32 insn) {
  return (insn >> 25) == 0x7D;
}

inline bool is_branch(u32 insn) {  // b, bl, blx <imm>
  return ((insn >> 25) & 7) == 5;
}

inline int32_t call_target(u32 insn, size_t at) {  // `at`: byte offset of the instruction
  int32_t t = (int32_t)(at + 8) + branch_offset(insn);
  return is_blx(insn) ? t + (int32_t)((insn >> 24) & 1) * 2 : t;
}

inline u32 make_call(size_t at, int32_t target, bool blx) {  // re-encodes a call of the same kind at a new position
  int32_t delta = target - (int32_t)(at + 8);
  if (!blx)
    return 0xEB000000 | ((delta / 4) & 0x00FFFFFF);
  return 0xFA000000 | (((delta >> 1) & 1) << 24) | ((delta >> 2) & 0x00FFFFFF);
}

inline u32 reg(u32 insn, int shift) {
  return (insn >> shift) & 15;
}

inline bool is_push_lr(u32 insn) {
  return (insn & 0xFFFF0000) == 0xE92D0000 && (insn & 0x4000);
}

inline bool is_r1_setup(u32 x) {  // mov r1,rS | add r1,rP,#imm | ldr r1,[rN,#+-imm]
  return (x & 0xFFFFFFF0) == 0xE1A01000 || (x & 0xFFF0FF00) == 0xE2801000 || (x & 0xFF70F000) == 0xE5101000;
}

struct Group {  // one string assignment: r1 = source, r0 = &field, call
  u32 r1, r0;
  size_t call;
};

struct Tail {
  size_t start, end;                // word indices: first setup instruction .. `str rB,[this,#S]`
  u32 this_reg;                     // optimised code only
  u32 src;                          // instruction that loads the sku into r1 (`mov r1,rS` or `add r1,rP,#imm`)
  u32 w, s;                         // m_watingTimes and state offsets
  std::vector<Group> groups;        // string assignments in order; the last one is the sku
  std::vector<u32> fillers;         // instructions between `mov rB,#3` and the state store
  bool o0 = false;                  // unoptimised build: every variable lives on the stack
  u32 this_off = 0, props_off = 0;  // o0: stack slots (`ldr r3,[r11,#-off]`) holding `this` and the product
};

// Optimised code. The block, in any scheduling order:
//   mov rA,#0 ; str rA,[T,#W] ; {add r0,T,#S+8 ; mov r1,.. ; bl}? ; add r0,T,#S+4 ; mov/add r1,.. ; bl ;
//   mov rB,#3 ; ... ; str rB,[T,#S]
inline std::vector<Tail> find_tail(const u32* w, size_t n) {
  std::vector<Tail> found;
  for (size_t e = 12; e < n; e++) {
    if ((w[e] & 0xFFF00000) != 0xE5800000)
      continue;
    u32 S = w[e] & 0xFFF, T = reg(w[e], 16), rb = reg(w[e], 12);
    if (S < 0x18 || S > 0x60)
      continue;
    size_t mb = 0;
    for (size_t k = e - 1; k >= e - 4; k--)
      if ((w[k] & 0xFFFF0FFF) == 0xE3A00003 && reg(w[k], 12) == rb) {
        mb = k;
        break;
      }
    if (!mb || !is_call(w[mb - 1]))
      continue;
    size_t c2 = mb - 1;
    u32 sku_add = 0xE2800000 | (S + 4) | (T << 16), ord_add = 0xE2800000 | (S + 8) | (T << 16);

    for (int groups = 1; groups <= 2; groups++) {
      size_t first = groups == 1 ? c2 - 4 : c2 - 7;
      size_t c1 = 0;
      int calls = 0;
      for (size_t k = first; k < c2; k++)
        if (is_call(w[k])) {
          c1 = k;
          calls++;
        }
      if (groups == 1 && calls != 0)
        continue;
      if (groups == 2 && calls != 1)
        continue;

      int zeros = 0, stores = 0;
      u32 A = 0;
      size_t store_idx = 0;
      std::vector<size_t> r1s_a, r1s_b, adds_a, adds_b;  // before / after c1 (everything is "b" when there is no c1)
      bool ok = true;
      for (size_t k = first; k < c2 && ok; k++) {
        if (groups == 2 && k == c1)
          continue;
        bool second = groups == 1 || k > c1;
        if ((w[k] & 0xFFFF0FFF) == 0xE3A00000) {
          zeros++;
          A = reg(w[k], 12);
        } else if ((w[k] & 0xFFF00000) == 0xE5800000 && reg(w[k], 16) == T) {
          stores++;
          store_idx = k;
        } else if (w[k] == sku_add)
          (second ? adds_b : adds_a).push_back(k);
        else if (groups == 2 && w[k] == ord_add)
          adds_a.push_back(k);
        else if (is_r1_setup(w[k]))
          (second ? r1s_b : r1s_a).push_back(k);
        else
          ok = false;
      }
      if (!ok || zeros != 1 || stores != 1 || reg(w[store_idx], 12) != A)
        continue;
      u32 W = w[store_idx] & 0xFFF;
      if (W != S + 8 && W != S + 12)
        continue;
      if (adds_b.size() != 1 || r1s_b.size() != 1)
        continue;
      if (groups == 2 && (adds_a.size() != 1 || r1s_a.size() != 1))
        continue;
      if (groups == 1 && !adds_a.empty())
        continue;

      Tail t{first, e, T, w[r1s_b[0]], W, S, {}, {}};
      if (groups == 2)
        t.groups.push_back({w[r1s_a[0]], w[adds_a[0]], c1});
      t.groups.push_back({w[r1s_b[0]], w[adds_b[0]], c2});
      bool plain = true;
      for (size_t k = mb + 1; k < e; k++) {
        if (is_branch(w[k]))
          plain = false;
        t.fillers.push_back(w[k]);
      }
      if (!plain)
        continue;
      found.push_back(t);
      break;
    }
  }
  return found;
}

// Unoptimised (-O0) form; r11 is the frame pointer and every value is reloaded from its stack slot:
//   ldr r3,[fp,#-T] ; mov r2,#0 ; str r2,[r3,#W] ; ldr r3,[fp,#-T] ; add r2,r3,#S+4 ; ldr r3,[fp,#-P] ;
//   add r3,r3,#imm ; mov r0,r2 ; mov r1,r3 ; bl assign ; ldr r3,[fp,#-T] ; mov r2,#3 ; str r2,[r3,#S]
inline std::vector<Tail> find_tail_o0(const u32* w, size_t n) {
  std::vector<Tail> found;
  for (size_t i = 0; i + 13 < n; i++) {
    u32 ldr_this = w[i];
    if ((ldr_this & 0xFFFFF000) != 0xE51B3000 || w[i + 1] != 0xE3A02000 || (w[i + 2] & 0xFFFFF000) != 0xE5832000)
      continue;
    u32 W = w[i + 2] & 0xFFF;
    u32 S = w[i + 12] & 0xFFF;
    if (S < 0x18 || (W != S + 8 && W != S + 12) || w[i + 3] != ldr_this || w[i + 4] != (0xE2832000 | (S + 4)) ||
        (w[i + 5] & 0xFFFFF000) != 0xE51B3000 || (w[i + 6] & 0xFFFFF000) != 0xE2833000 || w[i + 7] != 0xE1A00002 ||
        w[i + 8] != 0xE1A01003 || !is_call(w[i + 9]) || w[i + 10] != ldr_this || w[i + 11] != 0xE3A02003 ||
        w[i + 12] != (0xE5832000 | S))
      continue;
    Tail t{i, i + 12, 3, w[i + 6], W, S, {{w[i + 6], 0, i + 9}}, {}};
    t.o0 = true;
    t.this_off = ldr_this & 0xFFF;
    t.props_off = w[i + 5] & 0xFFF;
    found.push_back(t);
  }
  return found;
}

// push {..,lr} ... mov rX,#4|5 ; str rX,[rT,#S]  with a `ldr x,[r0,#player]` near the top: it bails out when there is
// no player, then moves to WaitingForValidate. `rT` is r0 or a copy of it.
inline std::vector<size_t> find_payment_complete(const u32* w, size_t n, u32 state_offset, u32 player_offset) {
  std::vector<size_t> found;
  for (size_t i = 24; i + 4 < n; i++) {
    if ((w[i] & 0xFFFF0FFF) != 0xE3A00004 && (w[i] & 0xFFFF0FFF) != 0xE3A00005)
      continue;
    u32 r = reg(w[i], 12);
    bool store = false;
    for (size_t m = 1; m <= 3; m++)
      if ((w[i + m] & 0xFFF0FFFF) == (0xE5800000 | state_offset | (r << 12)))
        store = true;
    if (!store)
      continue;
    for (size_t k = i - 1; k > i - 24; k--) {
      if (is_push_lr(w[k])) {
        bool player = false;
        for (size_t q = k + 1; q < k + 8 && q < i; q++)
          if ((w[q] & 0xFFF00FFF) == (0xE5900000 | player_offset) && reg(w[q], 16) == 0)
            player = true;
        if (player && (found.empty() || found.back() != k))
          found.push_back(k);
        break;
      }
    }
  }
  return found;
}

// Unoptimised form: push {..,lr} ; stores of r0-r2 ; ldr r3,[fp,#-x] ; ldr r3,[r3,#player] ; cmp r3,#0 ; ... ;
// mov r2,#4|5 ; str r2,[r3,#state]
inline std::vector<size_t> find_payment_complete_o0(const u32* w, size_t n, u32 state_offset, u32 player_offset) {
  std::vector<size_t> found;
  for (size_t i = 24; i + 2 < n; i++) {
    if ((w[i] != 0xE3A02004 && w[i] != 0xE3A02005) || w[i + 1] != (0xE5832000 | state_offset))
      continue;
    for (size_t k = i - 1; k > i - 24; k--) {
      if (is_push_lr(w[k])) {
        bool player = false;
        for (size_t q = k + 1; q + 1 < k + 14 && q < i; q++)
          if (w[q] == (0xE5933000 | player_offset) && w[q + 1] == 0xE3530000)
            player = true;
        if (player && (found.empty() || found.back() != k))
          found.push_back(k);
        break;
      }
    }
  }
  return found;
}

// Word indices of `add rY,pc,rX` instructions that resolve (through a literal-pool word) to `target`.
inline std::vector<size_t> find_xrefs(const u32* w, size_t n, u32 target) {
  std::vector<size_t> out;
  for (size_t i = 0; i < n; i++) {
    if ((w[i] & 0xFFFF0000) != 0xE59F0000)
      continue;
    u32 rd = reg(w[i], 12);
    size_t lit = (i * 4 + 8 + (w[i] & 0xFFF)) / 4;
    if (lit >= n)
      continue;
    for (size_t j = i + 1; j < i + 8 && j < n; j++) {
      if ((w[j] & 0x0FFF0000) == 0x008F0000 && (w[j] & 15) == rd && (u32)(j * 4 + 8 + w[lit]) == target)
        out.push_back(j);
    }
  }
  return out;
}

inline std::vector<size_t> find_wide_string(const uint8_t* image, size_t size, const char* text) {
  std::vector<uint8_t> pat;
  for (const char* c = text; *c; c++) {
    pat.push_back((uint8_t)*c);
    pat.insert(pat.end(), 3, 0);
  }
  std::vector<size_t> out;
  for (size_t i = 0; i + pat.size() <= size; i += 4)
    if (image[i] == pat[0] && !memcmp(image + i, pat.data(), pat.size()))
      out.push_back(i);
  return out;
}

inline void make_unconditional(u32& insn) {
  insn = (insn & 0x0FFFFFFF) | 0xE0000000;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Thumb-2 builds (3.5.5 and later are compiled to Thumb). Same sites, same idea, different encodings.
// ---------------------------------------------------------------------------------------------------------------
namespace thumb {

typedef uint8_t u8;
typedef uint16_t u16;

struct Ins {
  size_t addr;  // byte offset in the image
  u32 len;      // 2 or 4
  u16 h0, h1;
};

inline u16 rd16(const u8* img, size_t a) {
  return (u16)(img[a] | (img[a + 1] << 8));
}

inline u32 rd32(const u8* img, size_t a) {
  return img[a] | (img[a + 1] << 8) | (img[a + 2] << 16) | ((u32)img[a + 3] << 24);
}

inline void wr16(u8* img, size_t a, u16 v) {
  img[a] = (u8)v;
  img[a + 1] = (u8)(v >> 8);
}

inline bool is32(u16 h0) {
  return (h0 >> 11) >= 0x1D;
}

inline bool is_push_lr(const u8* img, size_t a) {
  u16 h = rd16(img, a);
  if ((h & 0xFF00) == 0xB500)
    return true;                                      // push {..,lr}
  return h == 0xE92D && (rd16(img, a + 2) & 0x4000);  // push.w {..,lr}
}

// Decodes forward from `from` until the instruction starting at or after `to`; returns false if `must` is not an
// instruction boundary (the decode desynchronised).
inline bool decode(const u8* img, size_t size, size_t from, size_t to, size_t must, std::vector<Ins>& out) {
  out.clear();
  bool hit = must == (size_t)-1;
  for (size_t a = from; a < to && a + 4 <= size;) {
    Ins i;
    i.addr = a;
    i.h0 = rd16(img, a);
    i.len = is32(i.h0) ? 4 : 2;
    i.h1 = i.len == 4 ? rd16(img, a + 2) : 0;
    if (a == must)
      hit = true;
    out.push_back(i);
    a += i.len;
  }
  return hit;
}

// ---- classification ----
inline bool is_call(const Ins& i) {  // bl / blx <imm>
  return i.len == 4 && (i.h0 & 0xF800) == 0xF000 && (i.h1 & 0xC000) == 0xC000 && ((i.h1 & 0x1000) || !(i.h1 & 1));
}

inline bool is_blx(const Ins& i) {
  return !(i.h1 & 0x1000);
}

inline int32_t call_offset(const Ins& i) {
  u32 S = (i.h0 >> 10) & 1, J1 = (i.h1 >> 13) & 1, J2 = (i.h1 >> 11) & 1;
  u32 I1 = ~(J1 ^ S) & 1, I2 = ~(J2 ^ S) & 1;
  u32 off = (S << 24) | (I1 << 23) | (I2 << 22) | ((u32)(i.h0 & 0x3FF) << 12) | ((u32)(i.h1 & 0x7FF) << 1);
  return (int32_t)(off << 7) >> 7;
}

inline int32_t call_target(const Ins& i) {
  size_t base = is_blx(i) ? ((i.addr + 4) & ~(size_t)3) : i.addr + 4;
  return (int32_t)base + call_offset(i);
}

inline void encode_call(size_t at, int32_t target, bool blx, u16& h0, u16& h1) {
  int32_t base = blx ? (int32_t)((at + 4) & ~(size_t)3) : (int32_t)(at + 4);
  int32_t off = target - base;
  u32 S = off < 0 ? 1 : 0, o = (u32)off & 0x1FFFFFF;
  u32 I1 = (o >> 23) & 1, I2 = (o >> 22) & 1, J1 = ~(I1 ^ S) & 1, J2 = ~(I2 ^ S) & 1;
  h0 = (u16)(0xF000 | (S << 10) | ((o >> 12) & 0x3FF));
  h1 = (u16)((blx ? 0xC000 : 0xD000) | (J1 << 13) | (J2 << 11) | ((o >> 1) & 0x7FF));
}

inline bool movs_imm(const Ins& i, u32 imm, u32* rd) {  // movs rd,#imm (16-bit)
  if (i.len == 2 && (i.h0 & 0xF800) == 0x2000 && (i.h0 & 0xFF) == imm) {
    *rd = (i.h0 >> 8) & 7;
    return true;
  }
  return false;
}

inline bool cmp_zero(const Ins& i, u32* rn) {
  if (i.len == 2 && (i.h0 & 0xF8FF) == 0x2800) {
    *rn = (i.h0 >> 8) & 7;
    return true;
  }
  return false;
}

// str rt,[rn,#imm]  (16-bit imm5 and .w imm12)
inline bool str_imm(const Ins& i, u32* rt, u32* rn, u32* imm) {
  if (i.len == 2 && (i.h0 & 0xF800) == 0x6000) {
    *rt = i.h0 & 7;
    *rn = (i.h0 >> 3) & 7;
    *imm = ((i.h0 >> 6) & 31) * 4;
    return true;
  }
  if (i.len == 4 && (i.h0 & 0xFFF0) == 0xF8C0) {
    *rt = i.h1 >> 12;
    *rn = i.h0 & 15;
    *imm = i.h1 & 0xFFF;
    return true;
  }
  return false;
}

inline bool ldrb_imm12(const Ins& i, u32* rt, u32* rn, u32* imm) {
  if (i.len == 4 && (i.h0 & 0xFFF0) == 0xF890) {
    *rt = i.h1 >> 12;
    *rn = i.h0 & 15;
    *imm = i.h1 & 0xFFF;
    return true;
  }
  return false;
}

// add rd,rn,#imm (ADDW T4, ADD.W T3 with a plain imm8, ADDS T1 imm3)
inline bool add_imm(const Ins& i, u32* rd, u32* rn, u32* imm) {
  if (i.len == 4 && (i.h0 & 0xFBF0) == 0xF200 && !(i.h1 & 0x8000)) {
    *rn = i.h0 & 15;
    *rd = (i.h1 >> 8) & 15;
    *imm = (((i.h0 >> 10) & 1) << 11) | (((i.h1 >> 12) & 7) << 8) | (i.h1 & 0xFF);
    return true;
  }
  if (i.len == 4 && (i.h0 & 0xFBE0) == 0xF100 && !(i.h1 & 0x8000) && (((i.h0 >> 10) & 1) == 0) &&
      (((i.h1 >> 12) & 7) == 0)) {
    *rn = i.h0 & 15;
    *rd = (i.h1 >> 8) & 15;
    *imm = i.h1 & 0xFF;
    return true;
  }
  if (i.len == 2 && (i.h0 & 0xFE00) == 0x1C00) {
    *rd = i.h0 & 7;
    *rn = (i.h0 >> 3) & 7;
    *imm = (i.h0 >> 6) & 7;
    return true;
  }
  return false;
}

// Sets the destination register of a "load r1" instruction to r2. Returns false for other instructions or if rd != 1.
inline bool r1_setup_to_r2(const Ins& i, u16& h0, u16& h1) {
  h0 = i.h0;
  h1 = i.h1;
  u32 rd, rn, imm;
  if (i.len == 2) {
    if ((i.h0 & 0xF800) == 0x9800 && ((i.h0 >> 8) & 7) == 1) {
      h0 = (u16)((i.h0 & 0xF8FF) | (2 << 8));
      return true;
    }  // ldr r1,[sp,#]
    if ((i.h0 & 0xF800) == 0x6800 && (i.h0 & 7) == 1) {
      h0 = (u16)((i.h0 & ~7) | 2);
      return true;
    }  // ldr r1,[rN,#]
    if ((i.h0 & 0xFF00) == 0x4600 && ((i.h0 >> 7) & 1) == 0 && (i.h0 & 7) == 1) {
      h0 = (u16)((i.h0 & ~7) | 2);
      return true;
    }  // mov r1,rM
    if ((i.h0 & 0xFE00) == 0x1C00 && (i.h0 & 7) == 1) {
      h0 = (u16)((i.h0 & ~7) | 2);
      return true;
    }  // adds r1,rN,#imm3
    return false;
  }
  if ((i.h0 & 0xFFF0) == 0xF8D0 && (i.h1 >> 12) == 1) {
    h1 = (u16)((i.h1 & 0x0FFF) | (2 << 12));
    return true;
  }  // ldr.w r1,[rN,#]
  if (add_imm(i, &rd, &rn, &imm) && rd == 1) {
    h1 = (u16)((i.h1 & ~0x0F00) | (2 << 8));
    return true;
  }  // add.w r1,rN,#imm
  return false;
}

inline bool is_r1_setup(const Ins& i) {
  u16 a, b;
  return r1_setup_to_r2(i, a, b);
}

// conditional branch (T1 16-bit or T3 32-bit): cond, absolute target
inline bool cond_branch(const Ins& i, u32* cond, int32_t* target) {
  if (i.len == 2 && (i.h0 & 0xF000) == 0xD000 && ((i.h0 >> 8) & 15) < 14) {
    *cond = (i.h0 >> 8) & 15;
    *target = (int32_t)i.addr + 4 + ((int32_t)(int8_t)(i.h0 & 0xFF)) * 2;
    return true;
  }
  if (i.len == 4 && (i.h0 & 0xF800) == 0xF000 && (i.h1 & 0xD000) == 0x8000 && ((i.h0 >> 6) & 15) < 14) {
    u32 S = (i.h0 >> 10) & 1, J1 = (i.h1 >> 13) & 1, J2 = (i.h1 >> 11) & 1;
    u32 off = (S << 20) | (J2 << 19) | (J1 << 18) | ((u32)(i.h0 & 0x3F) << 12) | ((u32)(i.h1 & 0x7FF) << 1);
    *cond = (i.h0 >> 6) & 15;
    *target = (int32_t)i.addr + 4 + ((int32_t)(off << 11) >> 11);
    return true;
  }
  return false;
}

// Makes the conditional branch unconditional (`b`) or removes it (nop). Returns false if the form is unsupported.
inline bool patch_branch(u8* img, const Ins& i, bool remove) {
  if (i.len == 2) {
    if (remove)
      wr16(img, i.addr, 0xBF00);
    else
      wr16(img, i.addr, (u16)(0xE000 | ((int32_t)(int8_t)(i.h0 & 0xFF) & 0x7FF)));
    return true;
  }
  if (remove) {
    wr16(img, i.addr, 0xBF00);
    wr16(img, i.addr + 2, 0xBF00);
    return true;
  }
  u32 cond;
  int32_t target;
  if (!cond_branch(i, &cond, &target))
    return false;
  int32_t off = target - ((int32_t)i.addr + 4);
  u32 S = off < 0 ? 1 : 0, o = (u32)off & 0x1FFFFFF;
  u32 I1 = (o >> 23) & 1, I2 = (o >> 22) & 1, J1 = ~(I1 ^ S) & 1, J2 = ~(I2 ^ S) & 1;
  wr16(img, i.addr, (u16)(0xF000 | (S << 10) | ((o >> 12) & 0x3FF)));
  wr16(img, i.addr + 2, (u16)(0x9000 | (J1 << 13) | (J2 << 11) | ((o >> 1) & 0x7FF)));
  return true;
}

// ---- cross-references (Thumb literal addressing) ----
inline std::vector<size_t> xrefs(const u8* img, size_t size, u32 target) {
  std::vector<size_t> out;
  size_t nh = size / 2;
  for (size_t i = 0; i + 8 < nh; i++) {
    u16 a = rd16(img, i * 2), b = rd16(img, i * 2 + 2);
    // movw rd,#lo ; movt rd,#hi ; add rd,pc
    if ((a & 0xFBF0) == 0xF240 && !(b & 0x8000)) {
      u32 rd = (b >> 8) & 15, lo = ((a & 15) << 12) | (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 0xFF);
      u16 c = rd16(img, i * 2 + 4), e = rd16(img, i * 2 + 6);
      if ((c & 0xFBF0) == 0xF2C0 && !(e & 0x8000) && ((e >> 8) & 15) == rd) {
        u32 hi = ((c & 15) << 12) | (((c >> 10) & 1) << 11) | (((e >> 12) & 7) << 8) | (e & 0xFF),
            val = (hi << 16) | lo;
        for (size_t k = i + 4; k < i + 9; k++) {
          u16 x = rd16(img, k * 2);
          if ((x & 0xFF78) == 0x4478 && (u32)((x & 7) | ((x >> 4) & 8)) == rd && (u32)(k * 2 + 4 + val) == target)
            out.push_back(k * 2);
        }
      }
    }
    // ldr rt,[pc,#imm] ; add rt,pc
    u32 rt = 99;
    size_t lit = 0, k0 = 0;
    if ((a & 0xF800) == 0x4800) {
      rt = (a >> 8) & 7;
      lit = (((i * 2 + 4) & ~(size_t)3)) + (a & 0xFF) * 4;
      k0 = i + 1;
    } else if (a == 0xF8DF || (a & 0xFF7F) == 0xF85F) {
      rt = b >> 12;
      u32 imm = b & 0xFFF;
      lit = ((i * 2 + 4) & ~(size_t)3) + ((a & 0x80) ? imm : (size_t)0 - imm);
      k0 = i + 2;
    }
    if (rt != 99 && lit + 4 <= size) {
      u32 v = rd32(img, lit);
      for (size_t k = k0; k < k0 + 6; k++) {
        u16 x = rd16(img, k * 2);
        if ((x & 0xFF78) == 0x4478 && (u32)((x & 7) | ((x >> 4) & 8)) == rt && (u32)(k * 2 + 4 + v) == target)
          out.push_back(k * 2);
      }
    }
  }
  return out;
}

// Start of the function containing `x`: the nearest preceding push {..,lr} from which a forward decode reaches `x`.
inline bool function_start(const u8* img, size_t size, size_t x, size_t max_back, size_t* start, std::vector<Ins>* ins,
                           size_t extra) {
  for (size_t a = x & ~(size_t)1; a + max_back > x && a >= 2; a -= 2) {
    if (!is_push_lr(img, a))
      continue;
    if (decode(img, size, a, x + extra, x, *ins)) {
      *start = a;
      return true;
    }
  }
  return false;
}

struct Group {
  size_t r1, r0, call;  // instruction indices
};

struct Found {
  std::vector<Ins> ins;
  size_t first = 0, store = 0;  // block: ins[first] .. ins[store]
  size_t mb = 0;                // `movs rB,#3`
  u32 T = 0, S = 0;
  std::vector<Group> groups;
  size_t fn_start = 0;
};

inline bool find_tail(const u8* img, size_t size, size_t x, Found& f) {
  size_t start;
  std::vector<Ins> ins;
  if (!function_start(img, size, x, 0x1000, &start, &ins, 0x400))
    return false;
  for (size_t e = 12; e < ins.size(); e++) {
    u32 rb, T, S;
    if (!str_imm(ins[e], &rb, &T, &S) || S < 0x18 || S > 0x100)
      continue;
    size_t mb = 0;
    for (size_t k = e - 1; k + 4 >= e && k > 8; k--) {
      u32 r;
      if (movs_imm(ins[k], 3, &r) && r == rb) {
        mb = k;
        break;
      }
    }
    if (!mb || !is_call(ins[mb - 1]))
      continue;
    size_t c2 = mb - 1;
    for (int groups = 1; groups <= 2; groups++) {
      size_t first = groups == 1 ? c2 - 4 : c2 - 7;
      size_t c1 = 0;
      int calls = 0;
      for (size_t k = first; k < c2; k++)
        if (is_call(ins[k])) {
          c1 = k;
          calls++;
        }
      if ((groups == 1 && calls != 0) || (groups == 2 && calls != 1))
        continue;
      int zeros = 0, stores = 0;
      u32 A = 0, zr = 0;
      size_t store_idx = 0;
      std::vector<size_t> r1a, r1b, aa, ab;
      bool ok = true;
      for (size_t k = first; k < c2 && ok; k++) {
        if (groups == 2 && k == c1)
          continue;
        bool second = groups == 1 || k > c1;
        u32 rt, rn, imm, rd;
        if (movs_imm(ins[k], 0, &zr)) {
          zeros++;
          A = zr;
        } else if (str_imm(ins[k], &rt, &rn, &imm) && rn == T) {
          stores++;
          store_idx = k;
          zr = rt;
        } else if (add_imm(ins[k], &rd, &rn, &imm) && rd == 0 && rn == T)
          (second ? ab : aa).push_back(k);
        else if (is_r1_setup(ins[k]))
          (second ? r1b : r1a).push_back(k);
        else
          ok = false;
      }
      if (!ok || zeros != 1 || stores != 1)
        continue;
      u32 rt, rn, imm;
      str_imm(ins[store_idx], &rt, &rn, &imm);
      if (rt != A)
        continue;
      if (ab.size() != 1 || r1b.size() != 1)
        continue;
      u32 rd2, rn2, sku;
      add_imm(ins[ab[0]], &rd2, &rn2, &sku);
      if (sku != S + 4)
        continue;
      if (groups == 2 && (aa.size() != 1 || r1a.size() != 1))
        continue;
      if (groups == 1 && !aa.empty())
        continue;
      f.ins = ins;
      f.first = first;
      f.store = e;
      f.mb = mb;
      f.T = T;
      f.S = S;
      f.fn_start = start;
      f.groups.clear();
      if (groups == 2)
        f.groups.push_back({r1a[0], aa[0], c1});
      f.groups.push_back({r1b[0], ab[0], c2});
      for (size_t k = mb + 1; k < e; k++)
        if (is_call(ins[k]))
          return false;
      return true;
    }
  }
  return false;
}

}  // namespace thumb

namespace thumb {

inline Result apply(uint8_t* img, size_t size) {
  Result r;
  // ---- onPurchaseConfirm tail, from the references to the dialog header ----
  std::vector<Found> tails;
  for (size_t s : detail::find_wide_string(img, size, "[PURCHASE_DIALOG_HEADER]")) {
    for (size_t x : xrefs(img, size, (u32)s)) {
      Found f;
      if (!find_tail(img, size, x, f))
        continue;
      bool dup = false;
      for (const Found& o : tails)
        dup |= o.ins[o.first].addr == f.ins[f.first].addr;
      if (!dup)
        tails.push_back(f);
    }
  }
  if (tails.size() != 1) {
    r.error = "onPurchaseConfirm tail not found exactly once";
    return r;
  }
  Found& t = tails[0];
  const Ins& first = t.ins[t.first];

  // ---- dialog check: ldrb.w x,[this,#F] ; cmp x,#0 ; beq tail   (or bne falling into the tail) ----
  size_t dialog = 0;
  bool dialog_nop = false;
  u32 F = 0;
  for (size_t b = t.first; b-- > 0 && !dialog;) {
    u32 cond;
    int32_t target;
    if (!cond_branch(t.ins[b], &cond, &target))
      continue;
    bool beq = cond == 0 && target == (int32_t)first.addr;
    bool bne = cond == 1 && b + 1 == t.first;
    if (!beq && !bne)
      continue;
    u32 rx;
    if (b == 0 || !cmp_zero(t.ins[b - 1], &rx))
      continue;
    for (size_t k = b - 1; k-- > 0 && b - k < 10;) {
      u32 rt, rn, imm;
      if (ldrb_imm12(t.ins[k], &rt, &rn, &imm) && rt == rx) {
        dialog = b;
        dialog_nop = bne;
        F = imm;
        break;
      }
    }
  }
  if (!dialog) {
    r.error = "dialog check not found";
    return r;
  }

  // ---- PurchaseSuccessed: same check, in the function that references the "confirmed" header ----
  std::vector<size_t> success;  // image offsets of the branch instructions
  for (size_t s : detail::find_wide_string(img, size, "[PURCHASE_DIALOG_CONFIRMED_HEADER]")) {
    for (size_t x : xrefs(img, size, (u32)s)) {
      // raw halfword search (robust against literal pools inside the function): ldrb.w x,[y,#F] ; cmp x,#0 ; beq/bne
      for (size_t a = x > 0x2000 ? x - 0x2000 : 0; a + 8 <= x; a += 2) {
        if ((rd16(img, a) & 0xFFF0) != 0xF890 || (rd16(img, a + 2) & 0xFFF) != F)
          continue;
        u32 rx = rd16(img, a + 2) >> 12;
        if (rx > 7 || rd16(img, a + 4) != (u16)(0x2800 | (rx << 8)))
          continue;
        Ins bi;
        bi.addr = a + 6;
        bi.h0 = rd16(img, a + 6);
        bi.len = is32(bi.h0) ? 4 : 2;
        bi.h1 = bi.len == 4 ? rd16(img, a + 8) : 0;
        u32 cond;
        int32_t target;
        if (cond_branch(bi, &cond, &target) && (cond == 0 || cond == 1))
          success.push_back(bi.addr);
      }
    }
  }
  std::sort(success.begin(), success.end());
  success.erase(std::unique(success.begin(), success.end()), success.end());
  if (success.empty() || success.size() > 2) {
    r.error = "PurchaseSuccessed dialog check not found once or twice";
    return r;
  }

  // ---- PurchaseDriverPaymentComplete: the function that owns a 120.0f literal and sets state 4/5 ----
  std::vector<size_t> completes;
  size_t lo = first.addr > 0x3000 ? first.addr - 0x3000 : 0;
  for (size_t p = lo & ~(size_t)3; p + 4 <= size && p < first.addr + 0x3000; p += 4) {
    if (rd32(img, p) != 0x42F00000)
      continue;
    size_t f = 0;
    for (size_t a = p; a > 2 && p - a < 0x600; a -= 2)
      if (is_push_lr(img, a)) {
        f = a;
        break;
      }
    if (!f)
      continue;
    bool state = false;
    for (size_t a = f; a < p; a += 2) {
      u16 h = rd16(img, a);
      if ((h & 0xF8FF) == 0x2004 || (h & 0xF8FF) == 0x2005)
        state = true;
    }
    if (state)
      completes.push_back(f);
  }
  std::sort(completes.begin(), completes.end());
  completes.erase(std::unique(completes.begin(), completes.end()), completes.end());
  if (completes.size() != 1) {
    r.error = "PurchaseDriverPaymentComplete not found exactly once";
    return r;
  }
  size_t complete = completes[0];

  // ---- the replacement block ----
  std::vector<u16> body;
  size_t at = first.addr;
  auto here = [&]() { return at + body.size() * 2; };
  auto put = [&](const Ins& i) {
    body.push_back(i.h0);
    if (i.len == 4)
      body.push_back(i.h1);
  };
  for (const Group& g : t.groups) {
    put(t.ins[g.r1]);
    put(t.ins[g.r0]);
    u16 h0, h1;
    encode_call(here(), call_target(t.ins[g.call]), is_blx(t.ins[g.call]), h0, h1);
    body.push_back(h0);
    body.push_back(h1);
  }
  body.push_back((u16)(0x4600 | (t.T << 3)));  // mov r0,this
  {
    u16 h0, h1;
    if (!r1_setup_to_r2(t.ins[t.groups.back().r1], h0, h1)) {
      r.error = "sku setup cannot be retargeted";
      return r;
    }
    body.push_back(h0);
    if (is32(h0))
      body.push_back(h1);
  }
  {
    u16 h0, h1;
    encode_call(here(), (int32_t)complete, false, h0, h1);
    body.push_back(h0);
    body.push_back(h1);
  }
  for (size_t k = t.mb + 1; k < t.store; k++)
    put(t.ins[k]);
  size_t length = (t.ins[t.store].addr + t.ins[t.store].len - first.addr) / 2;
  if (body.size() > length) {
    r.error = "tail does not fit";
    return r;
  }
  while (body.size() < length)
    body.push_back(0xBF00);

  // ---- apply ----
  if (!patch_branch(img, t.ins[dialog], dialog_nop)) {
    r.error = "dialog branch cannot be patched";
    return r;
  }
  for (size_t site : success) {
    Ins i;
    i.addr = site;
    i.h0 = rd16(img, site);
    i.len = is32(i.h0) ? 4 : 2;
    i.h1 = i.len == 4 ? rd16(img, site + 2) : 0;
    u32 cond;
    int32_t target;
    cond_branch(i, &cond, &target);
    if (!patch_branch(img, i, cond == 1)) {
      r.error = "success branch cannot be patched";
      return r;
    }
  }
  for (size_t k = 0; k < body.size(); k++)
    wr16(img, first.addr + k * 2, body[k]);

  r.applied = true;
  r.tail = (u32)first.addr;
  r.payment_complete = (u32)complete;
  r.dialog_check = (u32)t.ins[dialog].addr;
  r.success_dialog = (u32)success[0];
  return r;
}

}  // namespace thumb

// ARM-mode builds.
inline Result apply_arm(uint8_t* image, size_t size) {
  using namespace detail;
  Result r;
  u32* w = (u32*)image;
  size_t n = size / 4;

  std::vector<Tail> tails = find_tail(w, n);
  if (tails.empty())
    tails = find_tail_o0(w, n);
  if (tails.size() != 1) {
    r.error = "onPurchaseConfirm tail not found exactly once";
    return r;
  }
  Tail t = tails[0];
  if (t.start < 400) {
    r.error = "tail too close to the image start";
    return r;
  }
  // start of the function that contains the tail (nearest preceding `push {..,lr}`); searches below stay inside it
  size_t fn = t.start;
  while (fn > t.start - 400 && !is_push_lr(w[fn]))
    fn--;
  if (fn == t.start - 400) {
    r.error = "function start not found";
    return r;
  }

  std::vector<size_t> completes =
      t.o0 ? find_payment_complete_o0(w, n, t.s, t.w + 12) : find_payment_complete(w, n, t.s, t.w + 12);
  if (completes.size() != 1) {
    r.error = "PurchaseDriverPaymentComplete not found exactly once";
    return r;
  }
  size_t complete = completes[0];

  // dialog check inside onPurchaseConfirm: ldrb x,[this,#W+0x11] ... a branch that skips the dialog (beq -> tail) or
  // falls into the tail when not taken (bne)
  size_t dialog = 0;
  bool dialog_nop = false;
  for (size_t i = t.start - 1; i > fn && !dialog; i--) {
    if (t.o0 ? (w[i] & 0xFFF00FFF) != (0xE5D00000 | (t.w + 0x11))
             : (w[i] & 0xFFFF0FFF) != (0xE5D00000 | (t.w + 0x11) | (t.this_reg << 16)))
      continue;
    for (size_t j = i + 1; j < i + 8; j++) {
      if ((w[j] >> 24) == 0x0A && (int32_t)(j * 4 + 8) + branch_offset(w[j]) == (int32_t)(t.start * 4)) {
        dialog = j;
        break;
      }
      if ((w[j] >> 24) == 0x1A && j + 1 == t.start) {
        dialog = j;
        dialog_nop = true;
        break;
      }
    }
  }
  if (!dialog) {
    r.error = "dialog check not found";
    return r;
  }

  // pay-check gate (oldest builds, whose function takes the product id): blx rN ; subs rX,r0,#0 ; bne
  std::vector<size_t> services;
  for (size_t j = fn; j < t.start - 2; j++)
    if ((w[j] & 0xFFFFFFF0) == 0xE12FFF30 && (w[j + 1] & 0xFFF00FFF) == 0xE2500000 && reg(w[j + 1], 16) == 0 &&
        (w[j + 2] >> 24) == 0x1A)
      services.push_back(j + 2);
  // later builds take the product as a pointer (`add r1,rP,#imm`) and have no such gate
  bool props_based = t.o0 || (t.src & 0xFFF0FF00) == 0xE2801000;
  if (services.size() > 1 || (services.empty() && !props_based)) {
    r.error = "service check not found exactly once";
    return r;
  }

  // PurchaseSuccessed: `ldrb x,[y,#W+0x11]; cmp x,#0; b<cond>` in the function that references the "confirmed" header
  std::vector<size_t> success;
  for (size_t s : find_wide_string(image, size, "[PURCHASE_DIALOG_CONFIRMED_HEADER]")) {
    for (size_t x : find_xrefs(w, n, (u32)s)) {
      size_t f = x;
      while (f > 0 && x - f < 0x4000 && !is_push_lr(w[f]))
        f--;
      if (x - f >= 0x4000)
        continue;
      for (size_t i = f; i + 2 < x; i++) {
        if ((w[i] & 0xFFF00FFF) == (0xE5D00000 | (t.w + 0x11)) && (w[i + 1] & 0xFFF0FFFF) == 0xE3500000 &&
            reg(w[i + 1], 16) == reg(w[i], 12) && ((w[i + 2] >> 24) == 0x0A || (w[i + 2] >> 24) == 0x1A))
          success.push_back(i + 2);
      }
    }
  }
  std::sort(success.begin(), success.end());
  success.erase(std::unique(success.begin(), success.end()), success.end());
  // the compiler may have duplicated the check into both paths (1.0.x): patch every copy, but never an unexpected
  // number
  if (success.empty() || success.size() > 2) {
    r.error = "PurchaseSuccessed dialog check not found once or twice";
    return r;
  }
  size_t succ = success[0];

  // ---- everything located: build the replacement ----
  size_t length = t.end - t.start + 1;
  std::vector<u32> body;
  if (t.o0) {
    const Group& g = t.groups[0];
    size_t call = t.start + 9;
    u32 ldr_this = 0xE51B3000 | t.this_off, ldr_props = 0xE51B3000 | t.props_off;
    body = {ldr_this,
            0xE2830000 | (t.s + 4),
            ldr_props,
            (g.r1 & ~0xF000u) | 0x1000,
            make_call((t.start + 4) * 4, call_target(w[call], call * 4), is_blx(w[call])),
            0xE51B0000 | t.this_off,
            ldr_props,
            (g.r1 & ~0xF000u) | 0x2000,
            make_call((t.start + 8) * 4, (int32_t)(complete * 4), false)};
  } else {
    for (const Group& g : t.groups) {
      size_t at = t.start + body.size() + 2;
      body.push_back(g.r1);
      body.push_back(g.r0);
      body.push_back(make_call(at * 4, call_target(w[g.call], g.call * 4), is_blx(w[g.call])));
    }
    size_t at = t.start + body.size() + 2;
    body.push_back(0xE1A00000 | t.this_reg);
    body.push_back((t.src & ~0xF000u) | 0x2000);
    body.push_back(make_call(at * 4, (int32_t)(complete * 4), false));
    body.insert(body.end(), t.fillers.begin(), t.fillers.end());
  }
  if (body.size() > length) {
    r.error = "tail does not fit";
    return r;
  }
  while (body.size() < length)
    body.push_back(NOP);

  // ---- apply ----
  if (!services.empty())
    make_unconditional(w[services[0]]);
  if (dialog_nop)
    w[dialog] = NOP;
  else
    make_unconditional(w[dialog]);
  for (size_t site : success)
    if ((w[site] >> 24) == 0x1A)
      w[site] = NOP;
    else
      make_unconditional(w[site]);
  memcpy(w + t.start, body.data(), body.size() * 4);

  r.applied = true;
  r.tail = (u32)(t.start * 4);
  r.payment_complete = (u32)(complete * 4);
  r.service_check = services.empty() ? 0 : (u32)(services[0] * 4);
  r.dialog_check = (u32)(dialog * 4);
  r.success_dialog = (u32)(succ * 4);
  return r;
}

// `image`: the mapped library, offset 0 = first byte of the first PT_LOAD (guest base); `size` in bytes.
inline Result apply(uint8_t* image, size_t size) {
  Result r = apply_arm(image, size);
  if (!r.applied && !strcmp(r.error, "onPurchaseConfirm tail not found exactly once")) {
    Result t = thumb::apply(image, size);
    if (t.applied)
      return t;
    r.error = t.error;  // report why the Thumb attempt failed
  }
  return r;
}

}  // namespace free_shop
