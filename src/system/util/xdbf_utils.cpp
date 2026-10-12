/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/util/xdbf_utils.h>

#include <miniz.h>

namespace rex {
namespace system {
namespace util {

constexpr memory::fourcc_t kXdbfSignatureXdbf = memory::make_fourcc("XDBF");
constexpr memory::fourcc_t kXdbfSignatureXstc = memory::make_fourcc("XSTC");
constexpr memory::fourcc_t kXdbfSignatureXstr = memory::make_fourcc("XSTR");
constexpr memory::fourcc_t kXdbfSignatureXach = memory::make_fourcc("XACH");
constexpr memory::fourcc_t kXdbfSignatureXcxt = memory::make_fourcc("XCXT");
constexpr memory::fourcc_t kXdbfSignatureXsrc = memory::make_fourcc("XSRC");

constexpr uint64_t kXdbfIdTitle = 0x8000;
constexpr uint64_t kXdbfIdXstc = 0x58535443;
constexpr uint64_t kXdbfIdXach = 0x58414348;
constexpr uint64_t kXdbfIdXctx = 0x58435854;
constexpr uint64_t kXdbfIdXsrc = 0x58535243;

// XCXT and XSRC use the 12-byte section header (magic/version/size) and carry
// their own count or filename field, unlike XSTR/XACH which embed a u16 count
// (the 14-byte XdbfSectionHeader layout).
constexpr size_t kXdbfSectionHeaderBase = 12;

namespace {

uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Decompresses an RFC 1952 gzip stream by hand-parsing the header and handing
// the raw deflate payload to miniz (tinfl is self-terminating, so the trailing
// CRC32/ISIZE footer is ignored).
bool Gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
  if (size < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8) {
    return false;
  }
  const uint8_t flags = data[3];
  size_t pos = 10;

  if (flags & 0x04) {  // FEXTRA
    if (pos + 2 > size) {
      return false;
    }
    pos += 2 + static_cast<size_t>(data[pos] | (data[pos + 1] << 8));
  }
  auto skip_cstring = [&]() -> bool {
    while (pos < size && data[pos] != 0) {
      ++pos;
    }
    if (pos >= size) {
      return false;
    }
    ++pos;
    return true;
  };
  if ((flags & 0x08) && !skip_cstring()) {  // FNAME
    return false;
  }
  if ((flags & 0x10) && !skip_cstring()) {  // FCOMMENT
    return false;
  }
  if (flags & 0x02) {  // FHCRC
    pos += 2;
  }
  if (pos >= size) {
    return false;
  }

  size_t out_len = 0;
  void* decompressed = tinfl_decompress_mem_to_heap(data + pos, size - pos, &out_len, 0);
  if (!decompressed) {
    return false;
  }
  out.assign(static_cast<uint8_t*>(decompressed), static_cast<uint8_t*>(decompressed) + out_len);
  mz_free(decompressed);
  return true;
}

}  // namespace

XdbfWrapper::XdbfWrapper(const uint8_t* data, size_t data_size)
    : data_(data), data_size_(data_size) {
  if (!data || data_size <= sizeof(XbdfHeader)) {
    data_ = nullptr;
    return;
  }

  const uint8_t* ptr = data_;

  header_ = reinterpret_cast<const XbdfHeader*>(ptr);
  ptr += sizeof(XbdfHeader);
  if (header_->magic != kXdbfSignatureXdbf) {
    data_ = nullptr;
    return;
  }

  entries_ = reinterpret_cast<const XbdfEntry*>(ptr);
  ptr += sizeof(XbdfEntry) * header_->entry_count;

  files_ = reinterpret_cast<const XbdfFileLoc*>(ptr);
  ptr += sizeof(XbdfFileLoc) * header_->free_count;

  content_offset_ = ptr;
}

XdbfBlock XdbfWrapper::GetEntry(XdbfSection section, uint64_t id) const {
  for (uint32_t i = 0; i < header_->entry_used; ++i) {
    auto& entry = entries_[i];
    if (entry.section == static_cast<uint16_t>(section) && entry.id == id) {
      XdbfBlock block;
      block.buffer = content_offset_ + entry.offset;
      block.size = entry.size;
      return block;
    }
  }
  return {};
}

std::string XdbfWrapper::GetStringTableEntry(XLanguage language, uint16_t string_id) const {
  auto language_block = GetEntry(XdbfSection::kStringTable, static_cast<uint64_t>(language));
  if (!language_block) {
    return "";
  }

  auto xstr_head = reinterpret_cast<const XdbfSectionHeader*>(language_block.buffer);
  assert_true(xstr_head->magic == kXdbfSignatureXstr);
  assert_true(xstr_head->version == 1);

  const uint8_t* ptr = language_block.buffer + sizeof(XdbfSectionHeader);
  for (uint16_t i = 0; i < xstr_head->count; ++i) {
    auto entry = reinterpret_cast<const XdbfStringTableEntry*>(ptr);
    ptr += sizeof(XdbfStringTableEntry);
    if (entry->id == string_id) {
      return std::string(reinterpret_cast<const char*>(ptr), entry->string_length);
    }
    ptr += entry->string_length;
  }
  return "";
}

std::vector<XdbfAchievementTableEntry> XdbfWrapper::GetAchievements() const {
  std::vector<XdbfAchievementTableEntry> achievements;

  auto achievement_table = GetEntry(XdbfSection::kMetadata, kXdbfIdXach);
  if (!achievement_table) {
    return achievements;
  }

  auto xach_head = reinterpret_cast<const XdbfSectionHeader*>(achievement_table.buffer);
  assert_true(xach_head->magic == kXdbfSignatureXach);
  assert_true(xach_head->version == 1);

  const uint8_t* ptr = achievement_table.buffer + sizeof(XdbfSectionHeader);
  for (uint16_t i = 0; i < xach_head->count; ++i) {
    auto entry = reinterpret_cast<const XdbfAchievementTableEntry*>(ptr);
    ptr += sizeof(XdbfAchievementTableEntry);
    achievements.push_back(*entry);
  }
  return achievements;
}

std::vector<XdbfContextTableEntry> XdbfWrapper::GetContexts() const {
  std::vector<XdbfContextTableEntry> contexts;

  auto context_table = GetEntry(XdbfSection::kMetadata, kXdbfIdXctx);
  if (!context_table || context_table.size < kXdbfSectionHeaderBase + sizeof(uint32_t)) {
    return contexts;
  }

  auto xcxt_head = reinterpret_cast<const XdbfSectionHeader*>(context_table.buffer);
  assert_true(xcxt_head->magic == kXdbfSignatureXcxt);
  assert_true(xcxt_head->version == 1);

  const uint8_t* ptr = context_table.buffer + kXdbfSectionHeaderBase;
  const uint8_t* end = context_table.buffer + context_table.size;
  const uint32_t count = ReadBe32(ptr);
  ptr += sizeof(uint32_t);

  for (uint32_t i = 0; i < count; ++i) {
    if (ptr + sizeof(XdbfContextTableEntry) > end) {
      break;
    }
    contexts.push_back(*reinterpret_cast<const XdbfContextTableEntry*>(ptr));
    ptr += sizeof(XdbfContextTableEntry);
  }
  return contexts;
}

XLanguage XdbfGameData::GetExistingLanguage(XLanguage language_to_check) const {
  // A bit of a hack. Check if title in specific language exist.
  // If it doesn't then for sure language is not supported.
  return title(language_to_check).empty() ? default_language() : language_to_check;
}

XdbfBlock XdbfGameData::icon() const {
  return GetEntry(XdbfSection::kImage, kXdbfIdTitle);
}

XLanguage XdbfGameData::default_language() const {
  auto block = GetEntry(XdbfSection::kMetadata, kXdbfIdXstc);
  if (!block.buffer) {
    return XLanguage::kEnglish;
  }
  auto xstc = reinterpret_cast<const XdbfXstc*>(block.buffer);
  assert_true(xstc->magic == kXdbfSignatureXstc);
  return static_cast<XLanguage>(static_cast<uint32_t>(xstc->default_language));
}

std::string XdbfGameData::title() const {
  return GetStringTableEntry(default_language(), kXdbfIdTitle);
}

std::string XdbfGameData::title(XLanguage language) const {
  return GetStringTableEntry(language, kXdbfIdTitle);
}

std::vector<uint8_t> XdbfGameData::xlast() const {
  std::vector<uint8_t> out;

  auto xlast_table = GetEntry(XdbfSection::kMetadata, kXdbfIdXsrc);
  if (!xlast_table || xlast_table.size < kXdbfSectionHeaderBase + 2 * sizeof(uint32_t)) {
    return out;
  }

  auto xsrc_head = reinterpret_cast<const XdbfSectionHeader*>(xlast_table.buffer);
  assert_true(xsrc_head->magic == kXdbfSignatureXsrc);
  assert_true(xsrc_head->version == 1);

  const uint8_t* ptr = xlast_table.buffer + kXdbfSectionHeaderBase;
  const uint8_t* end = xlast_table.buffer + xlast_table.size;

  // u32 filename_length, filename, u32 decompressed_size, u32 compressed_size.
  const uint32_t filename_length = ReadBe32(ptr);
  ptr += sizeof(uint32_t) + filename_length;
  if (ptr + 2 * sizeof(uint32_t) > end) {
    return out;
  }
  const uint32_t compressed_size = ReadBe32(ptr + sizeof(uint32_t));
  ptr += 2 * sizeof(uint32_t);

  if (compressed_size == 0 || ptr + compressed_size > end) {
    return out;
  }

  Gunzip(ptr, compressed_size, out);
  return out;
}

}  // namespace util
}  // namespace system
}  // namespace rex
