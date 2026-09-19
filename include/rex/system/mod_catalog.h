/**
 * @file        system/mod_catalog.h
 * @brief       Public mod catalog client (Firestore REST, no auth) for the
 *              mod manager overlay's "All" tab.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Game-agnostic: no backend, project, or game identity is
 *              hardcoded into the request path itself; every piece of the
 *              endpoint is a cvar (declared in runtime.cpp), and the
 *              `catalog_name` this queries against comes from
 *              RuntimeConfig::catalog_name. Clearing the cvars/catalog_name
 *              disables the catalog exactly like a network failure would:
 *              state settles at kFailed, and the overlay simply omits the
 *              "All" tab. mod_catalog_fallback_url names a plain JSON
 *              catalog (see ParseFallbackCatalog) that is fetched instead
 *              whenever the Firestore query fails or is disabled, so a rate
 *              limited backend degrades to a static mirror, not to no tab. Nothing here ever throws
 * or blocks the UI thread -- FetchAsync/InstallAsync run on a worker std::thread (mirroring
 *              src/discord/discord_rpc.cpp's pattern; there's no thread
 *              pool), publishing into a mutex-guarded result the UI thread
 *              polls once per frame.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/net/http.h>

REXCVAR_DECLARE(std::string, mod_catalog_project);
REXCVAR_DECLARE(std::string, mod_catalog_api_key);
REXCVAR_DECLARE(std::string, mod_catalog_url);
REXCVAR_DECLARE(std::string, mod_catalog_games_collection);
REXCVAR_DECLARE(std::string, mod_catalog_mods_collection);
REXCVAR_DECLARE(std::string, mod_catalog_fallback_url);

namespace rex::system {

// One entry from the catalog's `mods` collection, per the CatalogMod schema
// documented alongside the endpoint (see docs/mod-system.md).
struct CatalogMod {
  std::string mod_id;
  std::string name;
  std::string author;
  std::string description;
  std::string version;
  std::string asset_url;
  std::string checksum;                // expected SHA-256 hex digest of the asset zip
  std::vector<std::string> platforms;  // empty = no platform restriction
  std::vector<std::string> requires_mods;
  std::string game_version;  // minimum host version; empty = unconstrained
  std::string icon_url;
  std::string status;  // "approved" | "featured" | ...
};

enum class CatalogState { kIdle, kLoading, kReady, kFailed };

// Outcome of InstallAsync, polled the same way as the fetch state.
struct CatalogInstallResult {
  bool in_progress = false;
  bool done = false;
  bool ok = false;
  // true if the install was staged rather than applied immediately (the mod
  // was already installed and possibly loaded this session); see
  // rex::system::ModState::StagePendingUpdate. Only meaningful when ok.
  bool staged = false;
  std::string message;
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
};

// Not copyable/movable (owns a worker thread); construct once and keep
// alive for the overlay's lifetime.
class ModCatalog {
 public:
  ModCatalog() = default;
  ~ModCatalog();

  ModCatalog(const ModCatalog&) = delete;
  ModCatalog& operator=(const ModCatalog&) = delete;

  // Starts (or restarts, if already kReady/kFailed) a background fetch of
  // `catalog_name`'s mod list. A no-op while already kLoading. Reads the
  // mod_catalog_* cvars and RuntimeConfig::catalog_name (via
  // rex::Runtime::instance()) at call time, so changing them and calling
  // Refresh() re-queries live. If `catalog_name` is empty, or both
  // mod_catalog_project, mod_catalog_url and mod_catalog_fallback_url are
  // empty, settles straight to kFailed without any network I/O.
  void Refresh();

  CatalogState state() const { return state_.load(std::memory_order_acquire); }

  // Snapshot of the last successful fetch (empty before the first kReady).
  // Safe to call from the UI thread at any time.
  std::vector<CatalogMod> Snapshot() const;

  // Starts a background download+verify+extract+install of `entry` into
  // `mods_root`, appending/refreshing its mods.toml entry and auto-sorting
  // afterward. `desired_id` is `entry.mod_id`. A no-op if an install is
  // already in progress. Never throws; failure (including a checksum
  // mismatch) is reported via InstallSnapshot(), never partially applied to
  // mods.toml.
  void InstallAsync(const CatalogMod& entry, const std::filesystem::path& mods_root);

  CatalogInstallResult InstallSnapshot() const;

  // The effective runQuery URL: mod_catalog_url if set, else assembled from
  // mod_catalog_project/mod_catalog_api_key. Empty if the catalog can't be
  // reached at all (see Refresh()). Exposed for tests.
  static std::string EffectiveQueryUrl();

 private:
  void FetchWorker(std::string catalog_name, std::string query_url, std::string games_collection,
                   std::string mods_collection, std::string fallback_url);
  bool FetchPrimary(const std::string& catalog_name, const std::string& query_url,
                    const std::string& games_collection, const std::string& mods_collection,
                    std::vector<CatalogMod>& out_mods);
  bool FetchFallback(const std::string& fallback_url, std::vector<CatalogMod>& out_mods);
  void InstallWorker(CatalogMod entry, std::filesystem::path mods_root);
  // Downloads+verifies+extracts+installs exactly one mod into mods_root and
  // records/refreshes its mods.toml entry. Shared by InstallWorker for both
  // the requested mod and any unmet `requires` dependency it pulls in first.
  // Never throws; returns false and fills `out_error` on any failure
  // (download, checksum mismatch, extract, filesystem). Sets `out_staged` to
  // true if `entry` was already installed and the new content was staged
  // (see rex::system::ModState::StagePendingUpdate) rather than written
  // straight to its live folder.
  bool InstallOneMod(const CatalogMod& entry, const std::filesystem::path& mods_root,
                     std::string& out_error, bool& out_staged);

  std::atomic<CatalogState> state_{CatalogState::kIdle};
  mutable std::mutex mods_mutex_;
  std::vector<CatalogMod> mods_;

  std::thread fetch_thread_;
  std::atomic<bool> fetch_in_flight_{false};
  // Aborts the in-flight request so the destructor's join returns now
  // instead of waiting out the HTTP timeout on the caller's thread.
  rex::net::CancelToken fetch_cancel_;

  std::thread install_thread_;
  std::atomic<bool> install_in_flight_{false};
  mutable std::mutex install_mutex_;
  CatalogInstallResult install_result_;
  rex::net::CancelToken install_cancel_;
};

// Parses a runQuery JSON response body into rows (skipping any element with
// no "document" key; the no-match shape is a one-element array with only
// "readTime"). Exposed for unit testing against canned fixtures.
std::vector<CatalogMod> ParseModsResponse(const std::string& json_body);

// Parses a plain JSON catalog (mod_catalog_fallback_url): either an array of
// mod objects or {"mods": [...]}, each object carrying the same camelCase
// keys as a Firestore CatalogMod document but as plain JSON values
// ("platform" and "requires" are string arrays). "status" defaults to
// "approved"; anything not approved/featured is dropped. Exposed for unit
// testing.
std::vector<CatalogMod> ParseFallbackCatalog(const std::string& json_body);

// Parses a games-collection runQuery response and returns the gameId (the
// last path segment of the first result's document.name), or empty if there
// was no match.
std::string ParseGameIdResponse(const std::string& json_body);

}  // namespace rex::system
