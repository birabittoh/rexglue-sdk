/**
 * @file        rex/system/util/presence_string_builder.h
 * @brief       Resolves {cN} specifiers in a rich presence template.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include <rex/system/util/xlast.h>

namespace rex::system::util {

// Resolves the {cN} (context) specifiers in a rich presence template against a
// user's context values, producing the display string XAM would show.
class PresenceStringBuilder {
 public:
  PresenceStringBuilder(std::string_view template_string, const XLast* xlast,
                        std::map<uint32_t, uint32_t> contexts,
                        XLanguage language = XLanguage::kEnglish);

  const std::string& presence_string() const { return presence_string_; }

 private:
  std::string ResolveSpecifier(std::string_view specifier) const;
  std::string ResolveContext(uint32_t context_id) const;

  const XLast* xlast_ = nullptr;
  std::map<uint32_t, uint32_t> contexts_;
  XLanguage language_ = XLanguage::kEnglish;
  std::string presence_string_;
};

}  // namespace rex::system::util
