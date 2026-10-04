/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <atomic>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/address_remap.h>
#include <rex/system/xmemory.h>

namespace rex::kernel::crt {

// ---------------------------------------------------------------------------
// Guest address remapping (rex/memory/address_remap.h)
// ---------------------------------------------------------------------------

namespace {

struct RemapState {
  memory::Memory* memory;
  std::vector<memory::GuestAddressRange> ranges;
  memory::GuestAddressRemap remap;
};

std::atomic<const RemapState*> g_remap{nullptr};

// The guest LR of the call being served, set by the hooks below.
thread_local uint32_t t_caller = 0;

// The guest address of `host`, if [host, host + n) touches a remapped range.
bool Remapped(const RemapState& state, const void* host, size_t n, uint32_t* guest) {
  const auto offset = static_cast<const uint8_t*>(host) - state.memory->virtual_membase();
  if (offset < 0 || offset > 0xFFFFFFFF)
    return false;
  const uint64_t start = static_cast<uint64_t>(offset);
  for (const auto& range : state.ranges) {
    if (start < range.end && start + n > range.start) {
      *guest = static_cast<uint32_t>(start);
      return true;
    }
  }
  return false;
}

struct Span {
  uint32_t start;
  uint32_t size;
  uint32_t caller;
};

uint8_t* RemappedByte(const RemapState& state, const Span& span, uint32_t i) {
  return state.memory->TranslateVirtual<uint8_t*>(
      state.remap(span.start + i, span.caller, span.start, span.size));
}

// Copies through the remap when either side touches a range. Buffered, so it
// is correct for overlapping spans too.
bool CopyRemapped(void* dst, const void* src, size_t n) {
  const RemapState* state = g_remap.load(std::memory_order_acquire);
  if (!state || !n)
    return false;
  uint32_t dst_guest = 0, src_guest = 0;
  const bool dst_remapped = Remapped(*state, dst, n, &dst_guest);
  const bool src_remapped = Remapped(*state, src, n, &src_guest);
  if (!dst_remapped && !src_remapped)
    return false;
  const Span dst_span{dst_guest, static_cast<uint32_t>(n), t_caller};
  const Span src_span{src_guest, static_cast<uint32_t>(n), t_caller | 1};
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) {
    bytes[i] = src_remapped ? *RemappedByte(*state, src_span, static_cast<uint32_t>(i))
                            : static_cast<const uint8_t*>(src)[i];
  }
  for (size_t i = 0; i < n; ++i) {
    uint8_t* to = dst_remapped ? RemappedByte(*state, dst_span, static_cast<uint32_t>(i))
                               : static_cast<uint8_t*>(dst) + i;
    *to = bytes[i];
  }
  return true;
}

bool SetRemapped(void* dst, int val, size_t n) {
  const RemapState* state = g_remap.load(std::memory_order_acquire);
  uint32_t guest = 0;
  if (!state || !n || !Remapped(*state, dst, n, &guest))
    return false;
  const Span span{guest, static_cast<uint32_t>(n), t_caller};
  for (size_t i = 0; i < n; ++i)
    *RemappedByte(*state, span, static_cast<uint32_t>(i)) = static_cast<uint8_t>(val);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

static void* native_memcpy(void* dst, const void* src, size_t n) {
  if (CopyRemapped(dst, src, n))
    return dst;
  return std::memcpy(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  if (CopyRemapped(dst, src, n))
    return dst;
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  if (SetRemapped(dst, val, n))
    return dst;
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  if (CopyRemapped(dst, src, n))
    return dst;
  return std::memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  if (SetRemapped(dst, val, n))
    return dst;
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  if (SetRemapped(dst, val, n))
    return dst;
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  if (SetRemapped(dst, val, n))
    return dst;
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  if (!CopyRemapped(dst, src, count))
    std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  if (!CopyRemapped(dst, src, count))
    std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

namespace rex::memory {

void SetGuestAddressRemap(Memory* memory, std::span<const GuestAddressRange> ranges,
                          GuestAddressRemap remap) {
  // Never freed: a copy on another thread may still be reading the old state.
  auto* state = new kernel::crt::RemapState{memory, {ranges.begin(), ranges.end()}, remap};
  kernel::crt::g_remap.store(state, std::memory_order_release);
}

}  // namespace rex::memory

// Like REX_HOOK, recording the caller for the remap handler.
#define REX_CRT_HOOK(subroutine, function)                      \
  extern "C" REX_FUNC(subroutine) {                             \
    rex::kernel::crt::t_caller = static_cast<uint32_t>(ctx.lr); \
    rex::ppc::HostToGuestFunction<function>(ctx, base);         \
  }

REX_CRT_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
REX_CRT_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
REX_CRT_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
REX_CRT_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
REX_CRT_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REX_CRT_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_CRT_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_CRT_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_CRT_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
