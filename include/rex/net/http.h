/**
 * @file        net/http.h
 * @brief       Minimal blocking HTTPS client surface.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Windows implementation uses WinHTTP (src/core/net/http_win.cpp);
 *              Linux (amd64 + arm64) links libcurl directly
 *              (src/core/net/http_curl.cpp), via find_package(CURL REQUIRED)
 *              so a Linux build fails fast rather than silently shipping
 *              without catalog support. Meant to be called from a worker
 *              thread (see rex::system::ModCatalog); every call here
 *              blocks. HTTPS-only: a non-https:// URL is rejected up front.
 *              Never throws; failures surface as a non-empty
 *              HttpResponse::error / a false return + populated `error`.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace rex::net {

// Aborts an in-flight blocking call from another thread. Without this a
// caller that wants to join its worker has to wait out the full connect /
// receive timeout (10-20s), which on a UI thread is a visible freeze.
// Cancelling is sticky: once cancelled, later calls handed the same token
// fail immediately instead of hitting the network.
class CancelToken {
 public:
  CancelToken() = default;
  CancelToken(const CancelToken&) = delete;
  CancelToken& operator=(const CancelToken&) = delete;

  // Thread safe, idempotent, and safe to call with no request in flight.
  void Cancel();
  bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

  // Backend use only. Attach hands the token ownership of the native handle
  // so exactly one of Cancel/CloseHandle tears it down under the lock;
  // returns false if the token was already cancelled (the handle is closed
  // and the request must not run). CloseHandle is the normal end-of-request
  // release.
  bool AttachHandle(void* handle);
  void CloseHandle();

 private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  void* handle_ = nullptr;
};

// Called periodically during HttpDownloadToFile with (bytes_downloaded,
// total_bytes; 0 if unknown by the server). May be empty.
using ProgressFn = std::function<void(uint64_t downloaded, uint64_t total)>;

struct HttpResponse {
  long status = 0;
  std::string body;
  // Non-empty on any failure (connect, TLS, timeout, non-https URL, ...);
  // `status` is meaningless when this is set.
  std::string error;

  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Blocking HTTPS GET. Rejects non-https:// URLs outright (error set, no
// network I/O attempted).
HttpResponse HttpGet(std::string_view url, const ProgressFn& progress = {},
                     CancelToken* cancel = nullptr);

// Blocking HTTPS POST with a JSON body ("Content-Type: application/json").
HttpResponse HttpPostJson(std::string_view url, std::string_view json_body,
                          CancelToken* cancel = nullptr);

// Blocking HTTPS download of `url` straight to `dest` (overwritten if it
// exists). Returns false and populates `error` on any failure (DNS, TLS,
// HTTP status >= 400, timeout, or a filesystem error writing `dest`). `dest`
// is left absent/partial-and-undefined on failure; callers should not treat
// a leftover file at `dest` as valid.
bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error,
                        CancelToken* cancel = nullptr);

}  // namespace rex::net
