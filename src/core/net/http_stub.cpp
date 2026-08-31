/**
 * @file        core/net/http_stub.cpp
 * @brief       Stub HTTP client for platforms without a native HTTP backend
 *              (Android). All requests return an error immediately.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/net/http.h>

namespace rex::net {

HttpResponse HttpGet(std::string_view /*url*/, const ProgressFn& /*progress*/) {
  return {0, {}, "HTTP client not available on this platform"};
}

HttpResponse HttpPostJson(std::string_view /*url*/, std::string_view /*json_body*/) {
  return {0, {}, "HTTP client not available on this platform"};
}

bool HttpDownloadToFile(std::string_view /*url*/, const std::filesystem::path& /*dest*/,
                        const ProgressFn& /*progress*/, std::string& error) {
  error = "HTTP client not available on this platform";
  return false;
}

}  // namespace rex::net
