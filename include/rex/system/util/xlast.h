/**
 * @file        rex/system/util/xlast.h
 * @brief       XLast (SPA title metadata) parser for localized strings and
 *              rich presence resolution.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/system/xcontent.h>

namespace rex::system::util {

// Parses a title's XLast XML (the decompressed XSRC entry of its SPA XDBF) to
// map context values to localized display strings and to build rich presence
// templates. The XML is UTF-16; it is decoded to UTF-8 on construction.
class XLast {
 public:
  XLast() = default;
  explicit XLast(std::vector<uint8_t> xml_bytes);

  bool HasXLast() const { return !xml_.empty(); }

  // Localized UTF-8 string for a string id, empty when undefined.
  std::string GetLocalizedString(uint32_t string_id, XLanguage language) const;

  // String id of the value of a title-defined context, if defined.
  std::optional<uint32_t> GetContextStringId(uint32_t context_id, uint32_t value) const;

  // String id of the presence template for a presence mode value.
  std::optional<uint32_t> GetPresenceStringId(uint32_t presence_mode_value) const;

  // Presence template (may contain {cN} specifiers) for a presence mode value.
  std::string GetPresenceRawString(uint32_t presence_mode_value, XLanguage language) const;

 private:
  std::string GetLocaleStringFromLanguage(XLanguage language) const;

  std::string xml_;
};

}  // namespace rex::system::util
