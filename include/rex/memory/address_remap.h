/**
 * @file        rex/memory/address_remap.h
 * @brief       Guest address remapping for the native CRT memory functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <span>

namespace rex::memory {

class Memory;

struct GuestAddressRange {
  uint32_t start;
  uint32_t end;  // exclusive
};

// Called per byte. `caller` is the guest return address of the call (its LR),
// with bit 0 set for the source side of a copy; [span, span + size) is the
// whole side of the call `address` belongs to.
using GuestAddressRemap = uint32_t (*)(uint32_t address, uint32_t caller, uint32_t span,
                                       uint32_t size);

// Recompiled code reaches a project's [address_remap] ranges through the
// handler codegen inlines, but rexcrt memcpy, memmove, memset and their
// variants run natively. Registering the same ranges here makes them copy or
// fill any span touching one byte by byte through `remap`.
void SetGuestAddressRemap(Memory* memory, std::span<const GuestAddressRange> ranges,
                          GuestAddressRemap remap);

}  // namespace rex::memory
