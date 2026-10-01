/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/filesystem.h>

namespace rex {
namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

std::filesystem::path ResolveRelativeTo(const std::filesystem::path& path,
                                        const std::filesystem::path& base) {
  if (path.empty() || path.is_absolute() || base.empty()) {
    return path;
  }
  return (base / path).lexically_normal();
}

std::filesystem::path RelativeIfInside(const std::filesystem::path& path,
                                       const std::filesystem::path& base) {
  if (path.empty() || base.empty()) {
    return path;
  }
  // Lexical on purpose: a junction inside base should stay relative even if it
  // points elsewhere, since moving base moves the link with it.
  std::error_code ec;
  auto full = std::filesystem::absolute(path, ec).lexically_normal();
  if (ec) {
    return path;
  }
  auto root = std::filesystem::absolute(base, ec).lexically_normal();
  if (ec) {
    return path;
  }
  auto rel = full.lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
    return path;
  }
  return rel;
}

}  // namespace filesystem
}  // namespace rex
