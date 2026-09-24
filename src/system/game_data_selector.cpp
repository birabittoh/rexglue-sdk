/**
 * @file        system/game_data_selector.cpp
 * @brief       GameDataSelector implementation — pre-presentation SDL native dialogs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/game_data_selector.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <span>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <miniz.h>
#include <picosha2.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xex_module.h>
#include <rex/ui/progress_window.h>

#if REX_PLATFORM_ANDROID
#include <rex/platform.h>
#endif

namespace rex::system {

// =============================================================================
// Android storage helpers
// =============================================================================

namespace {

/// Whether a native folder picker exists on this platform.
///
/// SDL's Android dialog backend answers SDL_FILEDIALOG_OPENFOLDER with
/// SDL_Unsupported() and fires the callback with a NULL file list, so
/// "Select Folder..." would show no picker at all and fail instantly. Even if it
/// did open, the Storage Access Framework hands back a `content://.../tree/...`
/// URI that std::filesystem cannot enumerate, so an extracted directory chosen
/// that way could not be read either. Extracted files are found by probing
/// GetPreExtractedCandidates() instead.
#if REX_PLATFORM_ANDROID
constexpr bool kCanPickFolder = false;
#else
constexpr bool kCanPickFolder = true;
#endif

/// Returns a writable base directory for game data, config, and updates.
/// On Android this is the app's internal storage (SDL_GetPrefPath, backed by
/// Context.getFilesDir()). On desktop it is the directory containing the
/// executable, matching the existing behaviour.
std::filesystem::path GetWritableBaseDir() {
#if REX_PLATFORM_ANDROID
  // SDL_GetPrefPath returns <internal-storage>/org/app/ (e.g.
  // /data/data/com.example.game/files/). The org/app arguments are only used
  // as fallbacks on desktop; on Android, SDL uses the package name from the
  // manifest and ignores them.
  const char* pref = SDL_GetPrefPath("rexglue", "eternalsonata");
  if (pref) {
    std::filesystem::path p(pref);
    SDL_free(const_cast<char*>(pref));
    return p;
  }
  // Absolute fallback — Android always has /sdcard writable by apps with
  // MANAGE_EXTERNAL_STORAGE or scoped storage access, but internal storage
  // through SDL should always work.
  return "/data/local/tmp";
#else
  return rex::filesystem::GetExecutableFolder();
#endif
}

/// Directories that may already hold an extracted game, in the order they are
/// probed before the wizard prompts for anything.
///
/// This is what makes an already-extracted copy usable on Android. There is no
/// folder picker there (see kCanPickFolder), so the only way to point the app
/// at extracted files is to put them where it already looks: its own internal
/// storage, or the app-specific external directory, which is world-writable
/// (`/sdcard/Android/data/<package>/files`) and therefore reachable over
/// adb/MTP without any storage permission.
///
/// On desktop this only re-discovers a previous extraction next to the
/// executable, which is harmless and saves a pointless trip through the dialog
/// when the config file was lost.
std::vector<std::filesystem::path> GetPreExtractedCandidates() {
  std::vector<std::filesystem::path> dirs;
  auto push = [&dirs](std::filesystem::path p) {
    if (p.empty()) {
      return;
    }
    if (std::find(dirs.begin(), dirs.end(), p) == dirs.end()) {
      dirs.push_back(std::move(p));
    }
  };

  push(GetWritableBaseDir() / "assets");
#if REX_PLATFORM_ANDROID
  // Both strings are cached statics owned by SDL, not to be freed.
  if (const char* internal_path = SDL_GetAndroidInternalStoragePath()) {
    push(std::filesystem::path(internal_path) / "assets");
  }
  if (const char* external_path = SDL_GetAndroidExternalStoragePath()) {
    push(std::filesystem::path(external_path) / "assets");
  }
#endif
  return dirs;
}

// =============================================================================
// Progress feedback for the long-running steps
// =============================================================================

/// Theme and icon for the progress window, set once from GameDataSelectorSettings
/// at the top of EnsureGameDataImpl. A global rather than a parameter thread
/// through ExtractIsoTo/ExtractXblaTo/ExtractTitleUpdateTo: those construct
/// their own ProgressReporter deep in extraction and have no other reason to
/// see the settings object.
ProgressWindowTheme g_progress_theme;
const void* g_progress_icon_data = nullptr;
size_t g_progress_icon_size = 0;
std::function<void(const std::string&, float, const std::string&)> g_progress_callback;

/// Progress for the steps that take a minute or more: disc/XBLA extraction and,
/// on Android, copying the picked content:// URI into the app's storage.
///
/// Owns the progress window for the duration of the step, redraws it at ~30 fps
/// as bytes go by, and mirrors a much slower summary into the log so headless
/// runs and bug reports still show the step advancing.
class ProgressReporter {
 public:
  /// `total_bytes` of 0 means "not known up front", which shows an
  /// indeterminate bar. Every caller supplies a real total; it stays supported
  /// because SDL_GetIOSize can fail on a stream that has no size.
  ProgressReporter(std::string label, uint64_t total_bytes)
      : label_(std::move(label)), total_bytes_(total_bytes) {
    if (!g_progress_callback) {
      window_.emplace("Preparing game files", g_progress_theme, g_progress_icon_data,
                      g_progress_icon_size);
    }
    Draw();
    Log();
  }

  ~ProgressReporter() {
    // Land on a full bar rather than leaving the last partial frame on screen
    // while the caller finishes up (hashing the extracted default.xex, mostly).
    if (total_bytes_ > 0) {
      bytes_ = total_bytes_;
    }
    Draw();
    Log();
  }

  ProgressReporter(const ProgressReporter&) = delete;
  ProgressReporter& operator=(const ProgressReporter&) = delete;

  /// Called per I/O chunk from FileReader::CopyTo (64 KiB) and from the
  /// content-URI copy (1 MiB); the throttles here decide the actual rates.
  void AddBytes(uint64_t n) {
    bytes_ += n;
    const uint64_t now = SDL_GetTicks();
    if (now - last_draw_ms_ >= kDrawIntervalMs) {
      Draw();
    }
    if (now - last_log_ms_ >= kLogIntervalMs) {
      Log();
    }
  }

 private:
  static constexpr uint64_t kDrawIntervalMs = 33;
  static constexpr uint64_t kLogIntervalMs = 2000;

  /// One decimal place, without pulling in iostream formatting.
  static std::string FormatBytes(uint64_t bytes) {
    constexpr uint64_t kMiB = 1024ull * 1024;
    constexpr uint64_t kGiB = kMiB * 1024;
    const bool gib = bytes >= kGiB;
    const uint64_t unit = gib ? kGiB : kMiB;
    const uint64_t tenths = (bytes * 10 + unit / 2) / unit;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
           (gib ? " GiB" : " MiB");
  }

  float Fraction() const {
    if (total_bytes_ == 0) {
      return -1.0f;
    }
    return std::min(
        1.0f, static_cast<float>(static_cast<double>(bytes_) / static_cast<double>(total_bytes_)));
  }

  std::string Detail() const {
    if (total_bytes_ == 0) {
      return FormatBytes(bytes_);
    }
    const uint64_t pct = std::min<uint64_t>(100, bytes_ * 100 / total_bytes_);
    return std::to_string(pct) + "%   " + FormatBytes(bytes_) + " of " + FormatBytes(total_bytes_);
  }

  void Draw() {
    last_draw_ms_ = SDL_GetTicks();
    if (g_progress_callback) {
      g_progress_callback(label_, Fraction(), Detail());
    } else {
      window_->Draw(label_, Fraction(), Detail());
    }
  }

  void Log() {
    last_log_ms_ = SDL_GetTicks();
    REXLOG_INFO("{}: {}", label_, Detail());
  }

  // Only when the app has nowhere of its own to draw progress.
  std::optional<rex::ui::ProgressWindow> window_;
  std::string label_;
  uint64_t total_bytes_ = 0;
  uint64_t bytes_ = 0;
  uint64_t last_draw_ms_ = 0;
  uint64_t last_log_ms_ = 0;
};

#if REX_PLATFORM_ANDROID
/// Test whether a path looks like an Android content:// URI.
bool IsContentUri(const std::string& path) {
  return path.size() > 10 && path.compare(0, 10, "content://") == 0;
}
#endif  // REX_PLATFORM_ANDROID

/// The one candidate directory the user can actually copy files into, or empty
/// if there is none.
///
/// Only the app's external files dir qualifies: `/data/data/...` is off limits
/// without root, so naming it in a prompt is noise. The `/storage/emulated/0`
/// prefix is shortened to `/sdcard`, because a message box has very little room
/// (SDL's Android backend pushes its buttons off screen if the text grows) and
/// the two are the same directory.
std::string UserReachableAssetsHint() {
#if REX_PLATFORM_ANDROID
  const char* external_path = SDL_GetAndroidExternalStoragePath();
  if (!external_path) {
    return {};
  }
  std::string p = (std::filesystem::path(external_path) / "assets").string();
  constexpr std::string_view kEmulated = "/storage/emulated/0";
  if (p.starts_with(kEmulated)) {
    p = "/sdcard" + p.substr(kEmulated.size());
  }
  return p;
#else
  return {};
#endif
}

#if REX_PLATFORM_ANDROID

/// Storage Access Framework tree URIs (`content://<authority>/tree/...`) name a
/// directory, not a file, so they can neither be copied with SDL_IOFromFile nor
/// walked with std::filesystem. Recognised only to reject them clearly.
bool IsContentTreeUri(const std::string& path) {
  return IsContentUri(path) && path.find("/tree/") != std::string::npos;
}

/// Earlier builds copied the picked disc image into `<base>/import` before
/// extracting it. Extraction now reads the content URI in place, so that copy
/// is dead weight: a second full-size duplicate of the image sitting in app
/// storage forever. Delete it once, on behalf of anyone upgrading.
void RemoveLegacyImportCopy() {
  auto import_dir = GetWritableBaseDir() / "import";
  std::error_code ec;
  if (!std::filesystem::exists(import_dir, ec)) {
    return;
  }
  const auto removed = std::filesystem::remove_all(import_dir, ec);
  if (ec) {
    REXLOG_WARN("Could not remove the legacy import copy at {}: {}", import_dir.string(),
                ec.message());
    return;
  }
  REXLOG_INFO("Removed the legacy import copy at {} ({} entries)", import_dir.string(), removed);
}
#endif  // REX_PLATFORM_ANDROID

}  // namespace

// =============================================================================
// XDVDFS extraction helpers (ported from scripts/extract_game.py)
// =============================================================================
namespace {

bool HexEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// =============================================================================
// Bounds-checked file access
// =============================================================================

/// Random access over a file that is fully untrusted: every offset and length
/// in an ISO or STFS package comes from header fields the user's file gets to
/// choose. Each accessor validates the requested range against the real file
/// size, so a corrupt or hostile field can only produce a failed read, never
/// an out-of-bounds access. Nothing is ever loaded wholesale, so a 16 GiB
/// image costs 16 GiB of I/O rather than 16 GiB of RAM.
class FileReader {
 public:
  explicit FileReader(const std::filesystem::path& path) {
#if REX_PLATFORM_ANDROID
    // A Storage Access Framework URI is not a file, so std::ifstream cannot
    // touch it; only SDL's IOStream knows how to resolve it through
    // ContentResolver. Reading the image in place through that handle is what
    // lets a multi-gigabyte ISO be extracted without first duplicating it into
    // the app's own storage.
    if (IsContentUri(path.string())) {
      io_ = SDL_IOFromFile(path.string().c_str(), "rb");
      if (!io_) {
        REXLOG_ERROR("Failed to open content URI {}: {}", path.string(), SDL_GetError());
        return;
      }
      const Sint64 io_size = SDL_GetIOSize(io_);
      if (io_size > 0) {
        size_ = static_cast<uint64_t>(io_size);
      } else {
        // Without a size every bounds check below would reject everything, so
        // treat it as a failed open rather than as an empty file.
        REXLOG_ERROR("Content URI {} reports no size: {}", path.string(), SDL_GetError());
      }
      return;
    }
#endif
    ifs_.open(path, std::ios::binary | std::ios::ate);
    if (ifs_) {
      auto end = ifs_.tellg();
      if (end > 0) {
        size_ = static_cast<uint64_t>(end);
      }
    }
  }

  ~FileReader() {
#if REX_PLATFORM_ANDROID
    if (io_) {
      SDL_CloseIO(io_);
    }
#endif
  }

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  bool ok() const { return size_ > 0; }
  uint64_t size() const { return size_; }

  /// Read exactly `len` bytes at `off`. Returns false if the range is not
  /// wholly inside the file or the read fails.
  bool Read(uint64_t off, void* dst, uint64_t len) {
    if (len == 0) {
      return true;
    }
    if (off > size_ || len > size_ - off) {
      return false;
    }
#if REX_PLATFORM_ANDROID
    if (io_) {
      if (SDL_SeekIO(io_, static_cast<Sint64>(off), SDL_IO_SEEK_SET) < 0) {
        return false;
      }
      auto* p = static_cast<char*>(dst);
      uint64_t remaining = len;
      while (remaining > 0) {
        // SDL_ReadIO is allowed to return short reads; 0 means EOF or error,
        // and the range was already bounds checked, so either is a failure.
        const size_t n = SDL_ReadIO(io_, p, static_cast<size_t>(remaining));
        if (n == 0) {
          return false;
        }
        p += n;
        remaining -= n;
      }
      return true;
    }
#endif
    ifs_.clear();
    ifs_.seekg(static_cast<std::streamoff>(off));
    if (!ifs_) {
      return false;
    }
    return static_cast<bool>(ifs_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len)));
  }

  std::optional<uint8_t> U8(uint64_t off) {
    uint8_t v;
    if (!Read(off, &v, 1))
      return std::nullopt;
    return v;
  }

  std::optional<uint16_t> U16BE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
  }

  std::optional<uint16_t> U16LE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  }

  std::optional<uint32_t> U24LE(uint64_t off) {
    uint8_t b[3];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16);
  }

  std::optional<uint32_t> U32BE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | b[3];
  }

  std::optional<uint32_t> U32LE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  }

  bool MagicAt(uint64_t off, std::string_view magic) {
    std::string buf(magic.size(), '\0');
    if (!Read(off, buf.data(), magic.size()))
      return false;
    return buf == magic;
  }

  /// Report bulk copies through `progress`, which every extraction path funnels
  /// into, so one hook covers ISO, XBLA and title-update extraction.
  void SetProgress(ProgressReporter* progress) { progress_ = progress; }

  /// Copy `len` bytes at `off` to `out` in chunks.
  bool CopyTo(std::ostream& out, uint64_t off, uint64_t len) {
    std::vector<char> chunk(64 * 1024);
    while (len > 0) {
      uint64_t n = std::min<uint64_t>(len, chunk.size());
      if (!Read(off, chunk.data(), n))
        return false;
      out.write(chunk.data(), static_cast<std::streamsize>(n));
      if (!out)
        return false;
      off += n;
      len -= n;
      if (progress_) {
        progress_->AddBytes(n);
      }
    }
    return true;
  }

 private:
  std::ifstream ifs_;
#if REX_PLATFORM_ANDROID
  SDL_IOStream* io_ = nullptr;
#endif
  uint64_t size_ = 0;
  ProgressReporter* progress_ = nullptr;
};

// =============================================================================
// Destination path safety
// =============================================================================

/// Join `base` with archive-supplied path components, refusing anything that
/// could escape the destination directory. Every name here is attacker
/// controlled: `..`, an absolute path, a drive letter, or an embedded
/// separator must never turn into a write outside `base`.
std::optional<std::filesystem::path> SafeJoin(const std::filesystem::path& base,
                                              std::span<const std::string> parts) {
  if (parts.empty()) {
    return std::nullopt;
  }
  std::filesystem::path rel;
  for (const std::string& part : parts) {
    if (part.empty() || part == "." || part == "..") {
      return std::nullopt;
    }
    if (part.find_first_of("/\\:") != std::string::npos) {
      return std::nullopt;
    }
    std::filesystem::path p(part);
    if (p.has_root_name() || p.has_root_directory()) {
      return std::nullopt;
    }
    // A single component must stay a single component.
    if (std::distance(p.begin(), p.end()) != 1) {
      return std::nullopt;
    }
    rel /= p;
  }

  auto root = base.lexically_normal();
  auto joined = (root / rel).lexically_normal();
  auto relative = joined.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    return std::nullopt;
  }
  return joined;
}

/// Result of an extraction: a count the caller can trust, plus whether any
/// entry failed. A partial extraction is a failure — the caller must not
/// report success for an install that is missing files.
struct ExtractResult {
  uint32_t files = 0;
  bool complete = true;
  /// Walk the tree without writing anything, just summing file sizes. Used for
  /// a measuring pass so the progress bar has a real total to work against.
  bool measure_only = false;
  uint64_t bytes = 0;
};

// =============================================================================
// XDVDFS (disc image) extraction — ported from scripts/extract_game.py
// =============================================================================

/// Game partition offsets seen in the wild; the volume descriptor sits 32
/// sectors into the partition.
constexpr uint64_t kXdvdfsPartitionOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                                0x0FD90000};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr uint64_t kXdvdfsSectorSize = 2048;

struct XdvdfsInfo {
  uint64_t game_offset;
  uint64_t root_offset;
  uint32_t root_size;
};

std::optional<XdvdfsInfo> FindXdvdfs(FileReader& reader) {
  for (uint64_t game_offset : kXdvdfsPartitionOffsets) {
    uint64_t fs_off = game_offset + 32 * kXdvdfsSectorSize;
    if (!reader.MagicAt(fs_off, kXdvdfsMagic)) {
      continue;
    }
    auto root_sector = reader.U32LE(fs_off + 20);
    auto root_size = reader.U32LE(fs_off + 24);
    if (!root_sector || !root_size) {
      continue;
    }
    if (*root_size < 13 || *root_size > 32 * 1024 * 1024) {
      continue;
    }
    return XdvdfsInfo{game_offset,
                      game_offset + static_cast<uint64_t>(*root_sector) * kXdvdfsSectorSize,
                      *root_size};
  }
  return std::nullopt;
}

constexpr int kMaxDirectoryDepth = 64;

void ExtractXdvdfsDirectory(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                            const std::filesystem::path& out_dir, int depth, ExtractResult& result);

/// Walk one directory's entry tree. The tree is a binary tree of ordinals
/// inside a single buffer; a malformed image can make it cyclic, so visited
/// ordinals are tracked instead of trusting it to terminate.
void ExtractXdvdfsEntries(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                          const std::filesystem::path& out_dir, int depth, ExtractResult& result) {
  std::vector<uint32_t> pending = {0};
  std::unordered_set<uint32_t> visited;

  while (!pending.empty()) {
    uint32_t ordinal = pending.back();
    pending.pop_back();
    if (!visited.insert(ordinal).second) {
      REXLOG_WARN("XDVDFS: cyclic directory entry (ordinal {}), skipping", ordinal);
      result.complete = false;
      continue;
    }

    uint64_t p = buffer_offset + static_cast<uint64_t>(ordinal) * 4;
    auto node_l = reader.U16LE(p);
    auto node_r = reader.U16LE(p + 2);
    auto sector = reader.U32LE(p + 4);
    auto length = reader.U32LE(p + 8);
    auto attributes = reader.U8(p + 12);
    auto name_length = reader.U8(p + 13);
    if (!node_l || !node_r || !sector || !length || !attributes || !name_length) {
      REXLOG_WARN("XDVDFS: truncated directory entry at 0x{:X}", p);
      result.complete = false;
      continue;
    }

    std::string name(*name_length, '\0');
    if (*name_length == 0 || !reader.Read(p + 14, name.data(), *name_length)) {
      REXLOG_WARN("XDVDFS: unreadable entry name at 0x{:X}", p);
      result.complete = false;
      continue;
    }

    if (*node_l) {
      pending.push_back(*node_l);
    }
    if (*node_r) {
      pending.push_back(*node_r);
    }

    const std::string name_parts[] = {name};
    auto dest = SafeJoin(out_dir, name_parts);
    if (!dest) {
      REXLOG_WARN("XDVDFS: rejected unsafe entry name '{}'", name);
      result.complete = false;
      continue;
    }

    uint64_t entry_offset = game_offset + static_cast<uint64_t>(*sector) * kXdvdfsSectorSize;

    if (*attributes & 0x10) {  // directory
      if (!result.measure_only) {
        std::filesystem::create_directories(*dest);
      }
      if (*length) {
        ExtractXdvdfsDirectory(reader, game_offset, entry_offset, *dest, depth + 1, result);
      }
      continue;
    }

    if (result.measure_only) {
      result.bytes += *length;
      ++result.files;
      continue;
    }

    std::ofstream ofs(*dest, std::ios::binary);
    if (!ofs) {
      REXLOG_WARN("XDVDFS: cannot write {}", dest->string());
      result.complete = false;
      continue;
    }
    if (!reader.CopyTo(ofs, entry_offset, *length)) {
      REXLOG_WARN("XDVDFS: truncated file data for '{}' ({} bytes at 0x{:X})", name, *length,
                  entry_offset);
      result.complete = false;
      continue;
    }
    ++result.files;
  }
}

void ExtractXdvdfsDirectory(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                            const std::filesystem::path& out_dir, int depth,
                            ExtractResult& result) {
  if (depth > kMaxDirectoryDepth) {
    REXLOG_WARN("XDVDFS: directory nesting deeper than {}, giving up", kMaxDirectoryDepth);
    result.complete = false;
    return;
  }
  ExtractXdvdfsEntries(reader, game_offset, buffer_offset, out_dir, depth, result);
}

/// Extract an XDVDFS disc image. Returns the number of files written, or 0 if
/// anything at all failed.
uint32_t ExtractIsoTo(const std::filesystem::path& iso_path, const std::filesystem::path& out_dir) {
  FileReader reader(iso_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open ISO: {}", iso_path.string());
    return 0;
  }

  auto info = FindXdvdfs(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid XDVDFS image: {}", iso_path.string());
    return 0;
  }

  std::filesystem::create_directories(out_dir);

  // Measure before extracting: the walk only reads directory sectors (a few
  // hundred KiB against a multi-gigabyte image), and it turns the progress bar
  // from indeterminate into a real percentage.
  ExtractResult measured;
  measured.measure_only = true;
  ExtractXdvdfsDirectory(reader, info->game_offset, info->root_offset, out_dir, 0, measured);
  REXLOG_INFO("ISO holds {} files, {} bytes", measured.files, measured.bytes);

  ExtractResult result;
  ProgressReporter progress("Extracting game files...", measured.bytes);
  reader.SetProgress(&progress);
  ExtractXdvdfsDirectory(reader, info->game_offset, info->root_offset, out_dir, 0, result);

  if (!result.complete) {
    REXLOG_ERROR("ISO extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  return result.files;
}

// =============================================================================
// STFS (LIVE/CON/PIRS package) — one implementation, used for both XBLA
// packages and title updates. Mirrors scripts/extract_tu.py, which in turn
// mirrors the SDK's StfsContainerDevice.
// =============================================================================

constexpr uint64_t kStfsBlockSize = 0x1000;
constexpr uint32_t kStfsEndOfChain = 0xFFFFFF;
constexpr int kStfsHashLevel0 = 170;
constexpr int kStfsHashLevel1 = 28900;  // 170^2

// Absolute offsets inside the StfsHeader (XContentHeader + XContentMetadata).
constexpr uint64_t kStfsOffHeaderSize = 0x340;        // be u32
constexpr uint64_t kStfsOffVolumeDescriptor = 0x379;  // StfsVolumeDescriptor
constexpr uint64_t kStfsOffVolumeType = 0x3A9;        // be u32, 0 = STFS, 1 = SVOD

struct StfsEntry {
  std::string name;
  bool is_directory = false;
  uint32_t start_block = 0;
  uint32_t parent = 0xFFFF;
  uint64_t size = 0;
};

struct StfsInfo {
  bool read_only_format = false;
  bool root_active_index = false;
  int blocks_per_hash_table = 1;
  int block_step[2] = {0, 0};
  uint32_t file_table_block_count = 0;
  uint32_t file_table_block_number = 0;
  uint32_t total_block_count = 0;
  uint64_t data_base = 0;
};

bool IsStfsMagic(FileReader& reader) {
  return reader.MagicAt(0, "LIVE") || reader.MagicAt(0, "CON ") || reader.MagicAt(0, "PIRS");
}

std::optional<StfsInfo> ParseStfsInfo(FileReader& reader) {
  if (!IsStfsMagic(reader)) {
    return std::nullopt;
  }

  auto header_size = reader.U32BE(kStfsOffHeaderSize);
  auto volume_type = reader.U32BE(kStfsOffVolumeType);
  const uint64_t vd = kStfsOffVolumeDescriptor;
  auto flags = reader.U8(vd + 2);
  auto file_table_block_count = reader.U16LE(vd + 3);
  auto file_table_block_number = reader.U24LE(vd + 5);
  auto total_block_count = reader.U32BE(vd + 0x1C);

  if (!header_size || !volume_type || !flags || !file_table_block_count ||
      !file_table_block_number || !total_block_count) {
    REXLOG_ERROR("STFS: header is truncated");
    return std::nullopt;
  }
  if (*volume_type != 0) {
    REXLOG_ERROR("STFS: SVOD packages are not supported");
    return std::nullopt;
  }

  StfsInfo info;
  info.read_only_format = (*flags & 0x1) != 0;
  info.root_active_index = ((*flags >> 1) & 0x1) != 0;
  info.file_table_block_count = *file_table_block_count;
  info.file_table_block_number = *file_table_block_number;
  info.total_block_count = *total_block_count;
  info.blocks_per_hash_table = info.read_only_format ? 1 : 2;
  info.block_step[0] = kStfsHashLevel0 + info.blocks_per_hash_table;
  info.block_step[1] = kStfsHashLevel1 + (kStfsHashLevel0 + 1) * info.blocks_per_hash_table;
  info.data_base = ((static_cast<uint64_t>(*header_size) + kStfsBlockSize - 1) / kStfsBlockSize) *
                   kStfsBlockSize;
  return info;
}

uint64_t StfsBlockToOffset(uint32_t block_index, const StfsInfo& info) {
  uint64_t block = block_index;
  int64_t base = kStfsHashLevel0;
  for (int i = 0; i < 3; ++i) {
    block += ((block_index + base) / base) * info.blocks_per_hash_table;
    if (static_cast<int64_t>(block_index) < base) {
      break;
    }
    base *= kStfsHashLevel0;
  }
  return info.data_base + (block << 12);
}

uint64_t StfsHashOffset(uint32_t block_index, int level, const StfsInfo& info) {
  int64_t block;
  if (level == 0) {
    if (static_cast<int64_t>(block_index) < kStfsHashLevel0) {
      block = 0;
    } else {
      block = static_cast<int64_t>(block_index / kStfsHashLevel0) * info.block_step[0];
      block +=
          (static_cast<int64_t>(block_index / kStfsHashLevel1) + 1) * info.blocks_per_hash_table;
      if (static_cast<int64_t>(block_index) >= kStfsHashLevel1) {
        block += info.blocks_per_hash_table;
      }
    }
  } else if (level == 1) {
    if (static_cast<int64_t>(block_index) < kStfsHashLevel1) {
      block = info.block_step[0];
    } else {
      block = static_cast<int64_t>(block_index / kStfsHashLevel1) * info.block_step[1] +
              info.blocks_per_hash_table;
    }
  } else {
    block = info.block_step[1];
  }
  return info.data_base + (static_cast<uint64_t>(block) << 12);
}

/// Follow the level-0 hash table to the next block of a chain. The hash
/// record's info word is *big-endian*, and which copy of a hash table is live
/// is resolved top-down through the levels that actually exist for this
/// package's block count.
std::optional<uint32_t> StfsNextBlock(FileReader& reader, uint32_t block_index,
                                      const StfsInfo& info) {
  uint64_t secondary = info.root_active_index ? kStfsBlockSize : 0;
  uint64_t off0 = StfsHashOffset(block_index, 0, info);

  if (info.read_only_format) {
    secondary = 0;
  } else {
    if (info.total_block_count > static_cast<uint32_t>(kStfsHashLevel0)) {
      uint64_t off1 = StfsHashOffset(block_index, 1, info);
      if (info.total_block_count > static_cast<uint32_t>(kStfsHashLevel1)) {
        uint64_t off2 = StfsHashOffset(block_index, 2, info);
        uint32_t rec2 = (block_index / kStfsHashLevel1) % kStfsHashLevel0;
        auto word = reader.U32BE(off2 + secondary + rec2 * 0x18 + 0x14);
        if (!word)
          return std::nullopt;
        secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
      }
      uint32_t rec1 = (block_index / kStfsHashLevel0) % kStfsHashLevel0;
      auto word = reader.U32BE(off1 + secondary + rec1 * 0x18 + 0x14);
      if (!word)
        return std::nullopt;
      secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
    }
  }

  uint32_t rec0 = block_index % kStfsHashLevel0;
  auto word = reader.U32BE(off0 + secondary + rec0 * 0x18 + 0x14);
  if (!word)
    return std::nullopt;
  return *word & 0xFFFFFF;
}

std::vector<StfsEntry> ParseStfsFileTable(FileReader& reader, const StfsInfo& info) {
  std::vector<StfsEntry> entries;
  uint32_t table_block = info.file_table_block_number;

  for (uint32_t bi = 0; bi < info.file_table_block_count; ++bi) {
    if (table_block == kStfsEndOfChain) {
      break;
    }
    uint64_t block_off = StfsBlockToOffset(table_block, info);

    for (uint64_t m = 0; m < kStfsBlockSize / 0x40; ++m) {
      uint64_t off = block_off + m * 0x40;

      auto first = reader.U8(off);
      if (!first || *first == 0) {
        break;
      }
      auto name_flags = reader.U8(off + 0x28);
      if (!name_flags) {
        break;
      }
      uint8_t name_len = *name_flags & 0x3F;
      if (name_len == 0) {
        break;
      }

      std::string name(name_len, '\0');
      auto start_block = reader.U24LE(off + 0x2F);
      auto parent = reader.U16BE(off + 0x32);
      auto size = reader.U32BE(off + 0x34);
      if (!reader.Read(off, name.data(), name_len) || !start_block || !parent || !size) {
        break;
      }

      StfsEntry entry;
      entry.name = std::move(name);
      entry.is_directory = (*name_flags & 0x80) != 0;
      entry.start_block = *start_block;
      entry.parent = *parent;
      entry.size = *size;
      entries.push_back(std::move(entry));
    }

    auto next = StfsNextBlock(reader, table_block, info);
    if (!next) {
      REXLOG_WARN("STFS: file table chain broke after block {}", table_block);
      break;
    }
    table_block = *next;
  }
  return entries;
}

/// Stream one STFS file's block chain to `out`.
bool StfsCopyFile(FileReader& reader, const StfsEntry& entry, const StfsInfo& info,
                  std::ostream& out) {
  uint32_t block_index = entry.start_block;
  uint64_t remaining = entry.size;
  uint64_t guard = 0;
  const uint64_t max_blocks = static_cast<uint64_t>(info.total_block_count) + 1;

  while (remaining > 0 && block_index != kStfsEndOfChain) {
    if (++guard > max_blocks) {
      REXLOG_WARN("STFS: block chain for '{}' is cyclic", entry.name);
      return false;
    }
    uint64_t n = std::min<uint64_t>(remaining, kStfsBlockSize);
    if (!reader.CopyTo(out, StfsBlockToOffset(block_index, info), n)) {
      return false;
    }
    remaining -= n;
    if (remaining > 0) {
      auto next = StfsNextBlock(reader, block_index, info);
      if (!next) {
        return false;
      }
      block_index = *next;
    }
  }

  if (remaining > 0) {
    REXLOG_WARN("STFS: chain for '{}' ended {} bytes short", entry.name, remaining);
    return false;
  }
  return true;
}

/// Read a whole STFS file into memory. Only used for files small enough to
/// inspect (the XEX delta patch), never for bulk extraction.
bool StfsReadFile(FileReader& reader, const StfsEntry& entry, const StfsInfo& info,
                  std::string& out) {
  constexpr uint64_t kMaxInMemory = 256ull * 1024 * 1024;
  if (entry.size > kMaxInMemory) {
    REXLOG_WARN("STFS: '{}' is too large to inspect ({} bytes)", entry.name, entry.size);
    return false;
  }
  std::ostringstream oss(std::ios::binary);
  if (!StfsCopyFile(reader, entry, info, oss)) {
    return false;
  }
  out = oss.str();
  return true;
}

/// Resolve an entry's full path components by walking its parent chain.
std::optional<std::vector<std::string>> StfsEntryPath(const std::vector<StfsEntry>& entries,
                                                      size_t index) {
  std::vector<std::string> parts = {entries[index].name};
  uint32_t p = entries[index].parent;
  int depth = 0;
  while (p != 0xFFFF) {
    if (p >= entries.size() || ++depth > 100) {
      return std::nullopt;
    }
    parts.push_back(entries[p].name);
    p = entries[p].parent;
  }
  std::reverse(parts.begin(), parts.end());
  return parts;
}

/// Total bytes of the regular files in an STFS package, for the progress bar.
/// The file table is already parsed by the time this is needed, so unlike the
/// disc path no measuring walk is required.
uint64_t SumStfsFileBytes(const std::vector<StfsEntry>& entries) {
  uint64_t total = 0;
  for (const auto& entry : entries) {
    if (!entry.is_directory) {
      total += entry.size;
    }
  }
  return total;
}

/// Extract every file in an STFS package, preserving directory layout.
/// `skip` lets a caller exclude an entry it has already written itself.
ExtractResult ExtractStfsTree(FileReader& reader, const std::vector<StfsEntry>& entries,
                              const StfsInfo& info, const std::filesystem::path& out_dir,
                              const StfsEntry* skip) {
  ExtractResult result;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if (entry.is_directory || &entry == skip) {
      continue;
    }

    auto parts = StfsEntryPath(entries, i);
    if (!parts) {
      REXLOG_WARN("STFS: invalid parent chain for entry '{}'", entry.name);
      result.complete = false;
      continue;
    }
    auto dest = SafeJoin(out_dir, *parts);
    if (!dest) {
      REXLOG_WARN("STFS: rejected unsafe entry path for '{}'", entry.name);
      result.complete = false;
      continue;
    }

    std::filesystem::create_directories(dest->parent_path());
    std::ofstream ofs(*dest, std::ios::binary);
    if (!ofs) {
      REXLOG_WARN("STFS: cannot write {}", dest->string());
      result.complete = false;
      continue;
    }
    if (!StfsCopyFile(reader, entry, info, ofs)) {
      REXLOG_WARN("STFS: failed to read '{}'", entry.name);
      result.complete = false;
      continue;
    }
    ++result.files;
  }
  return result;
}

/// Extract an XBLA package. Uses the same STFS implementation as the title
/// update path, so CON/PIRS and writable (2-block hash table) packages work
/// here too.
uint32_t ExtractXblaTo(const std::filesystem::path& xbla_path,
                       const std::filesystem::path& out_dir) {
  FileReader reader(xbla_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open XBLA: {}", xbla_path.string());
    return 0;
  }

  auto info = ParseStfsInfo(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid LIVE/CON/PIRS package: {}", xbla_path.string());
    return 0;
  }

  auto entries = ParseStfsFileTable(reader, *info);
  if (entries.empty()) {
    REXLOG_ERROR("No STFS entries found in XBLA: {}", xbla_path.string());
    return 0;
  }
  REXLOG_INFO("Extracting {} STFS entries from {}", entries.size(), xbla_path.string());

  std::filesystem::create_directories(out_dir);
  ProgressReporter progress("Extracting game files...", SumStfsFileBytes(entries));
  reader.SetProgress(&progress);
  auto result = ExtractStfsTree(reader, entries, *info, out_dir, nullptr);

  if (!result.complete) {
    REXLOG_ERROR("XBLA extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  if (!std::filesystem::is_regular_file(out_dir / "default.xex")) {
    REXLOG_ERROR("XBLA extraction did not produce a root default.xex");
    return 0;
  }
  return result.files;
}

// =============================================================================
// Title update extraction
// =============================================================================

/// Find the XEX delta patch inside a title update. The patch is *not*
/// necessarily the largest file in the package — updates routinely ship
/// replacement media that is bigger — so prefer entries named `*.xexp` and
/// fall back to trying files largest-first, accepting the first one that
/// actually begins with the XEX2 magic.
std::optional<size_t> FindXexpEntry(FileReader& reader, const std::vector<StfsEntry>& entries,
                                    const StfsInfo& info, std::string& xexp_data) {
  std::vector<size_t> candidates;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].is_directory || entries[i].size < 4) {
      continue;
    }
    std::string lower = entries[i].name;
    for (auto& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".xexp") == 0) {
      candidates.push_back(i);
    }
  }

  if (candidates.empty()) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!entries[i].is_directory && entries[i].size >= 4) {
        candidates.push_back(i);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](size_t a, size_t b) { return entries[a].size > entries[b].size; });
  }

  for (size_t i : candidates) {
    std::string data;
    if (!StfsReadFile(reader, entries[i], info, data)) {
      continue;
    }
    if (data.size() >= 4 && data.compare(0, 4, "XEX2") == 0) {
      xexp_data = std::move(data);
      return i;
    }
    REXLOG_DEBUG("TU: '{}' is not an XEX2 delta patch, trying next", entries[i].name);
  }
  return std::nullopt;
}

/// Extract a title update. The package is extracted whole into `update_dir`,
/// layout preserved, to be mounted as the `update:` device; the XEX delta
/// patch is then *copied* to `<xexp_dir>/default.xexp`, next to the base
/// default.xex it patches. It stays in the update tree as well, so that tree
/// remains a faithful copy of the package.
///
/// The two trees must not be merged: a title update commonly replaces data
/// files that exist in the base game under the same relative paths, so
/// extracting it over the game root would silently overwrite base game files
/// in a directory the user may have extracted themselves.
///
/// Returns the number of files written, or 0 on failure.
uint32_t ExtractTitleUpdateTo(const std::filesystem::path& tu_path,
                              const std::filesystem::path& xexp_dir,
                              const std::filesystem::path& update_dir) {
  FileReader reader(tu_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open TU: {}", tu_path.string());
    return 0;
  }

  auto info = ParseStfsInfo(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid LIVE/CON/PIRS package: {}", tu_path.string());
    return 0;
  }

  auto entries = ParseStfsFileTable(reader, *info);
  if (entries.empty()) {
    REXLOG_ERROR("No entries found in TU: {}", tu_path.string());
    return 0;
  }
  REXLOG_INFO("TU has {} entries", entries.size());

  std::string xexp_data;
  auto xexp_index = FindXexpEntry(reader, entries, *info, xexp_data);
  if (!xexp_index) {
    REXLOG_ERROR("No XEX delta patch (XEX2) found inside {}", tu_path.string());
    return 0;
  }
  REXLOG_INFO("TU delta patch: '{}' ({} bytes)", entries[*xexp_index].name, xexp_data.size());

  // Extract the package whole, delta patch included.
  std::filesystem::create_directories(update_dir);
  ProgressReporter progress("Extracting title update...", SumStfsFileBytes(entries));
  reader.SetProgress(&progress);
  auto result = ExtractStfsTree(reader, entries, *info, update_dir, nullptr);
  if (!result.complete) {
    REXLOG_ERROR("TU extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  REXLOG_INFO("TU extracted: {} files to {}", result.files, update_dir.string());

  // Then place a copy of the delta patch next to the base default.xex.
  std::filesystem::create_directories(xexp_dir);
  auto xexp_path = xexp_dir / "default.xexp";
  {
    std::ofstream ofs(xexp_path, std::ios::binary);
    if (!ofs) {
      REXLOG_ERROR("Cannot write {}", xexp_path.string());
      return 0;
    }
    ofs.write(xexp_data.data(), static_cast<std::streamsize>(xexp_data.size()));
    if (!ofs) {
      REXLOG_ERROR("Failed writing {}", xexp_path.string());
      return 0;
    }
  }
  REXLOG_INFO("Delta patch copied to {}", xexp_path.string());

  return result.files;
}

// ---------------------------------------------------------------------------
// Native dialog helpers
// ---------------------------------------------------------------------------

/// Lowercase hex SHA-256 of a file's contents, or empty on a read failure.
std::string Sha256File(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::vector<unsigned char> hash(picosha2::k_digest_size);
  picosha2::hash256(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(),
                    hash.begin(), hash.end());
  return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

/// Hex SHA-256 of a byte range.
std::string Sha256Hex(const uint8_t* data, size_t size) {
  std::vector<unsigned char> hash(picosha2::k_digest_size);
  picosha2::hash256(data, data + size, hash.begin(), hash.end());
  return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

/// A default_xex_patches entry, header parsed; see GameDataSelectorSettings.
struct XexPatch {
  std::string source_file_sha256;
  std::string source_sha256;
  std::string target_sha256;
  uint64_t source_size = 0;
  uint64_t target_size = 0;
  uint64_t control_count = 0;
  uint64_t diff_size = 0;
  uint64_t extra_size = 0;
  std::span<const uint8_t> payload;
};

constexpr size_t kXexPatchHeaderSize = 0x8C;

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | p[i];
  return v;
}

std::optional<XexPatch> ParseXexPatch(std::span<const uint8_t> data) {
  if (data.size() < kXexPatchHeaderSize || std::memcmp(data.data(), "RXD1", 4) != 0) {
    return std::nullopt;
  }
  XexPatch patch;
  patch.source_file_sha256 =
      picosha2::bytes_to_hex_string(data.begin() + 0x04, data.begin() + 0x24);
  patch.source_sha256 = picosha2::bytes_to_hex_string(data.begin() + 0x24, data.begin() + 0x44);
  patch.target_sha256 = picosha2::bytes_to_hex_string(data.begin() + 0x44, data.begin() + 0x64);
  patch.source_size = ReadLE64(data.data() + 0x64);
  patch.target_size = ReadLE64(data.data() + 0x6C);
  patch.control_count = ReadLE64(data.data() + 0x74);
  patch.diff_size = ReadLE64(data.data() + 0x7C);
  patch.extra_size = ReadLE64(data.data() + 0x84);
  patch.payload = data.subspan(kXexPatchHeaderSize);
  return patch;
}

/// The form patches work on: the original headers with encryption and
/// compression switched off, followed by the flat image, which the loader
/// takes as is. Must match the project's gen-xex-patch.py.
bool NormalizeXex(const std::vector<uint8_t>& xex, std::vector<uint8_t>& out) {
  std::vector<uint8_t> image;
  if (!rex::runtime::XexModule::ExtractBaseImage(xex.data(), xex.size(), image)) {
    return false;
  }
  auto be32 = [&](size_t off) {
    return uint32_t(xex[off]) << 24 | uint32_t(xex[off + 1]) << 16 | uint32_t(xex[off + 2]) << 8 |
           uint32_t(xex[off + 3]);
  };
  const uint32_t header_size = be32(8);
  const uint32_t header_count = be32(20);
  out.assign(xex.begin(), xex.begin() + header_size);
  for (uint32_t i = 0; i < header_count && 24 + 8 * i + 8 <= header_size; ++i) {
    if (be32(24 + 8 * i) != XEX_HEADER_FILE_FORMAT_INFO) {
      continue;
    }
    const uint32_t info = be32(24 + 8 * i + 4);
    if (info + 8 > header_size) {
      return false;
    }
    std::memset(out.data() + info + 4, 0, 4);  // encryption and compression types
    out.insert(out.end(), image.begin(), image.end());
    return true;
  }
  return false;
}

bool ApplyXexPatch(const XexPatch& patch, const std::vector<uint8_t>& source,
                   std::vector<uint8_t>& out) {
  const uint64_t control_size = patch.control_count * 24;
  std::vector<uint8_t> payload(control_size + patch.diff_size + patch.extra_size);
  mz_ulong length = mz_ulong(payload.size());
  if (mz_uncompress(payload.data(), &length, patch.payload.data(),
                    mz_ulong(patch.payload.size())) != MZ_OK ||
      length != payload.size()) {
    return false;
  }
  const uint8_t* diff = payload.data() + control_size;
  const uint8_t* extra = diff + patch.diff_size;
  out.clear();
  out.reserve(patch.target_size);
  int64_t s = 0;
  uint64_t d = 0, e = 0;
  for (uint64_t n = 0; n < patch.control_count; ++n) {
    const int64_t add = int64_t(ReadLE64(payload.data() + n * 24));
    const int64_t copy = int64_t(ReadLE64(payload.data() + n * 24 + 8));
    const int64_t seek = int64_t(ReadLE64(payload.data() + n * 24 + 16));
    if (add < 0 || copy < 0 || d + add > patch.diff_size || e + copy > patch.extra_size || s < 0 ||
        uint64_t(s + add) > source.size()) {
      return false;
    }
    for (int64_t i = 0; i < add; ++i)
      out.push_back(uint8_t(source[s + i] + diff[d + i]));
    s += add;
    d += add;
    out.insert(out.end(), extra + e, extra + e + copy);
    e += copy;
    s += seek;
  }
  return out.size() == patch.target_size;
}

/// Rewrites a recognised other release's default.xex into the pinned one,
/// keeping the original next to it as default.xex.orig.
bool ConvertXex(const std::filesystem::path& xex_path, const XexPatch& patch) {
  std::vector<uint8_t> xex;
  {
    std::ifstream in(xex_path, std::ios::binary);
    xex.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::vector<uint8_t> source, target;
  if (!NormalizeXex(xex, source) || source.size() != patch.source_size ||
      !HexEqual(Sha256Hex(source.data(), source.size()), patch.source_sha256)) {
    REXLOG_ERROR("default.xex could not be normalised for conversion");
    return false;
  }
  if (!ApplyXexPatch(patch, source, target) ||
      !HexEqual(Sha256Hex(target.data(), target.size()), patch.target_sha256)) {
    REXLOG_ERROR("default.xex conversion produced the wrong image");
    return false;
  }
  const auto tmp_path = std::filesystem::path(xex_path).concat(".tmp");
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(target.data()), std::streamsize(target.size()));
    if (!out) {
      REXLOG_ERROR("Could not write {}", tmp_path.string());
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(xex_path, std::filesystem::path(xex_path).concat(".orig"), ec);
  if (!ec) {
    std::filesystem::rename(tmp_path, xex_path, ec);
  }
  if (ec) {
    REXLOG_ERROR("Could not install the converted default.xex: {}", ec.message());
    return false;
  }
  return true;
}

/// Check that default.xex exists at dir and, if a hash is pinned, that it is
/// the pinned release, one already converted from a patch's source release, or
/// a patch's source release, which is then converted in place.
bool ValidateDefaultXexInDir(const std::filesystem::path& dir,
                             const GameDataSelectorSettings& settings) {
  auto xex_path = dir / "default.xex";
  if (!std::filesystem::is_regular_file(xex_path)) {
    REXLOG_ERROR("default.xex not found in {}", dir.string());
    return false;
  }
  const std::string& expected = settings.default_xex_sha256;
  if (expected.empty()) {
    return true;
  }
  std::string actual = Sha256File(xex_path);
  if (HexEqual(actual, expected)) {
    REXLOG_INFO("default.xex SHA-256 verified OK");
    return true;
  }
  for (const auto data : settings.default_xex_patches) {
    const auto patch = ParseXexPatch(data);
    if (!patch) {
      REXLOG_ERROR("Ignoring a malformed default.xex patch");
      continue;
    }
    if (HexEqual(actual, patch->target_sha256)) {
      REXLOG_INFO("default.xex SHA-256 verified OK (converted from another release)");
      return true;
    }
    if (HexEqual(actual, patch->source_file_sha256)) {
      REXLOG_INFO("default.xex is from another release; converting it");
      if (!ConvertXex(xex_path, *patch)) {
        return false;
      }
      REXLOG_INFO("default.xex converted; the original is kept as default.xex.orig");
      return true;
    }
  }
  REXLOG_ERROR("default.xex SHA-256 mismatch: expected={}, actual={}", expected, actual);
  return false;
}

/// Case-insensitive extension check.
bool HasExtension(const std::filesystem::path& p, std::string_view ext) {
  auto e = p.extension().string();
  for (auto& c : e) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return e == "." + std::string(ext);
}

/// STFS container (XBLA package, title update, ...). Content files inside an
/// unpacked XBLA dump have no extension — GUID-like names such as
/// `EB9399B31E4A9181AA7F8F6B7F6ABD3263DDA24058` — so sniff the magic instead
/// of trusting the file name.
bool IsStfsPackage(const std::filesystem::path& p) {
  FileReader reader(p);
  return reader.ok() && IsStfsMagic(reader);
}

/// XDVDFS disc image, identified by the same partition probe the extractor
/// uses, so sniffing and extraction can never disagree.
bool IsXdvdfsImage(const std::filesystem::path& p) {
  FileReader reader(p);
  return reader.ok() && FindXdvdfs(reader).has_value();
}

/// SDL file-dialog callback stores the first selected path in a caller-owned
/// struct, then signals completion.
/// The callback may run on a different thread than the caller (the Windows
/// IFileOpenDialog backend runs the dialog on its own thread), so `done` is
/// atomic and is published last.
struct FileDialogState {
  std::string path;
  bool error = false;
  std::atomic<bool> done{false};
};

void SDLCALL FileDialogCallback(void* userdata, const char* const* files, int /*filter_index*/) {
  auto* state = static_cast<FileDialogState*>(userdata);
  if (!files) {
    // NULL file list means the dialog failed to run, not that the user
    // cancelled (cancel gives an empty, non-NULL list).
    state->error = true;
  } else if (files[0]) {
    state->path = files[0];
  }
  state->done.store(true, std::memory_order_release);
}

/// Call SDL_ShowOpenFileDialog and pump events until the dialog closes.
/// Handles both synchronous (Windows COM) and async (Linux portal) backends.
bool RunFileDialog(std::string& out_path, std::span<const SDL_DialogFileFilter> filters) {
  FileDialogState state;
  SDL_ShowOpenFileDialog(FileDialogCallback, &state, nullptr, filters.data(),
                         static_cast<int>(filters.size()), nullptr, false);

  // Pump SDL events until the dialog completes.
  // On synchronous platforms (Windows) `done` will already be true here.
  // On async platforms (Linux portal) the callback is triggered by SDL's
  // event loop, so we must pump until it arrives.
  while (!state.done.load(std::memory_order_acquire)) {
    // Pumping allows the portal callback to fire. Not draining: the app window
    // may already exist, and the resize it gets when the dialog closes (Android
    // hands the activity a new surface) must still reach it.
    SDL_PumpEvents();
    SDL_Delay(16);
  }

  if (state.error) {
    REXLOG_ERROR("SDL_ShowOpenFileDialog failed: {}", SDL_GetError());
    return false;
  }
  if (state.path.empty()) {
    REXLOG_INFO("File dialog cancelled by user");
    return false;
  }
  out_path = std::move(state.path);
  return true;
}

/// Same as RunFileDialog, but picks a directory instead of a file.
bool RunFolderDialog(std::string& out_path) {
  FileDialogState state;
  SDL_ShowOpenFolderDialog(FileDialogCallback, &state, nullptr, nullptr, false);

  while (!state.done.load(std::memory_order_acquire)) {
    SDL_PumpEvents();  // See RunFileDialog.
    SDL_Delay(16);
  }

  if (state.error) {
    REXLOG_ERROR("SDL_ShowOpenFolderDialog failed: {}", SDL_GetError());
    return false;
  }
  if (state.path.empty()) {
    REXLOG_INFO("Folder dialog cancelled by user");
    return false;
  }
  out_path = std::move(state.path);
  return true;
}

/// Show a native info message box with up to three buttons and return the
/// index of the one the user clicked (-1 on failure). The last button is the
/// escape/cancel action.
/// Note: SDL takes NUL-terminated strings here, so these helpers deliberately
/// take `const std::string&` / `const char*` rather than string_view — a view
/// of a substring would be passed on unterminated.
int ShowInfoBox(const std::string& title, const std::string& message,
                std::span<const char* const> button_labels) {
  SDL_MessageBoxButtonData buttons[3];
  int nbuttons = 0;

  for (const char* label : button_labels) {
    if (!label || !*label || nbuttons == 3) {
      continue;
    }
    buttons[nbuttons] = {0, nbuttons, label};
    ++nbuttons;
  }
  if (nbuttons > 0) {
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[nbuttons - 1].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
  }

  SDL_MessageBoxData mb = {SDL_MESSAGEBOX_INFORMATION,
                           nullptr,
                           title.c_str(),
                           message.c_str(),
                           nbuttons,
                           buttons,
                           nullptr};
  int button_id = -1;
  if (!SDL_ShowMessageBox(&mb, &button_id)) {
    REXLOG_ERROR("SDL_ShowMessageBox failed: {}", SDL_GetError());
    return -1;
  }
  return button_id;
}

/// Show a native error message box.
void ShowErrorBox(const std::string& title, const std::string& message) {
  SDL_MessageBoxButtonData ok = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"};
  SDL_MessageBoxData mb = {
      SDL_MESSAGEBOX_ERROR, nullptr, title.c_str(), message.c_str(), 1, &ok, nullptr};
  int dummy;
  SDL_ShowMessageBox(&mb, &dummy);
}

/// Config file the resolved game_data_root is written back to: the caller's
/// config_path if it set one, otherwise `<exe stem>.toml` next to the
/// executable (which is how ReXApp names it).
std::filesystem::path ResolveConfigPath(const GameDataSelectorSettings& settings) {
  if (!settings.config_path.empty()) {
    return settings.config_path;
  }
#if REX_PLATFORM_ANDROID
  return GetWritableBaseDir() / "config.toml";
#else
  auto exe_path = rex::filesystem::GetExecutablePath();
  if (exe_path.empty()) {
    REXLOG_WARN("No config_path given and the executable path is unknown; not persisting");
    return {};
  }
  return rex::filesystem::GetExecutableFolder() / (exe_path.stem().string() + ".toml");
#endif
}

/// Write game_data_root (and update_data_root, if one is in use) back to the
/// config file, patching just those lines and leaving the rest untouched.
void PersistGameDataRoot(const GameDataSelectorSettings& settings) {
  auto config_path = ResolveConfigPath(settings);
  if (config_path.empty()) {
    return;
  }
  std::vector<std::string> names = {"game_data_root"};
  if (!std::string(REXCVAR_GET(update_data_root)).empty()) {
    names.push_back("update_data_root");
  }
  rex::cvar::SaveConfigSubset(config_path, names);
}

}  // namespace

// =============================================================================
// Validation
// =============================================================================

static bool IsGameDataValid(std::string_view game_data_root,
                            const GameDataSelectorSettings& settings) {
  if (game_data_root.empty()) {
    return false;
  }
  std::filesystem::path dir(game_data_root);
  if (!std::filesystem::is_directory(dir)) {
    return false;
  }
  return ValidateDefaultXexInDir(dir, settings);
}

// =============================================================================
// Title update helper
// =============================================================================

/// Where the title update's files are extracted, to be mounted as the
/// `update:` device. Honours an update_data_root the user has already
/// configured; otherwise defaults to `update` next to the executable.
static std::filesystem::path ResolveUpdateDir() {
  std::string udr = REXCVAR_GET(update_data_root);
  if (!udr.empty()) {
    return std::filesystem::path(udr);
  }
  return GetWritableBaseDir() / "update";
}

static bool ProcessTitleUpdate(const std::filesystem::path& dir,
                               const GameDataSelectorSettings& settings) {
  auto xexp_path = dir / "default.xexp";
  auto update_dir = ResolveUpdateDir();

  // This build does not patch: a default.xexp left over from a build that did
  // would still be picked up by the loader, so remove the copy next to
  // default.xex. The extracted update tree is left alone — it is the user's
  // data, and switching back to a TU build should not need a re-extraction.
  if (settings.title_update_sha256.empty()) {
    if (std::filesystem::is_regular_file(xexp_path)) {
      std::error_code ec;
      std::filesystem::remove(xexp_path, ec);
      if (ec) {
        REXLOG_WARN("This build needs no title update, but {} could not be removed: {}",
                    xexp_path.string(), ec.message());
      } else {
        REXLOG_INFO("Removed stale {} (this build needs no title update)", xexp_path.string());
      }
    }
    return true;
  }

  // The patch alone is not enough: the extracted update tree is what gets
  // mounted as the update: device, so a missing update dir means the update
  // has to be extracted even though default.xexp is sitting there.
  if (std::filesystem::is_regular_file(xexp_path) && std::filesystem::is_directory(update_dir)) {
    REXLOG_INFO("default.xexp and the extracted update at {} are both present, skipping TU prompt",
                update_dir.string());
    REXCVAR_SET(update_data_root, update_dir.string());
    return true;
  }

  // The patch is missing next to default.xex but the update may already have
  // been extracted on an earlier run — restore the copy from there rather than
  // asking for the package again.
  {
    auto src = update_dir / "default.xexp";
    if (std::filesystem::is_regular_file(src)) {
      std::error_code ec;
      std::filesystem::copy_file(src, xexp_path, std::filesystem::copy_options::overwrite_existing,
                                 ec);
      if (ec) {
        REXLOG_ERROR("Failed to copy {} to {}: {}", src.string(), xexp_path.string(), ec.message());
        // Fall through to the prompt rather than failing outright.
      } else {
        REXLOG_INFO("Restored default.xexp from the extracted update at {}", update_dir.string());
        REXCVAR_SET(update_data_root, update_dir.string());
        return true;
      }
    } else {
      REXLOG_INFO("No extracted update at {}, prompting for the title update package",
                  update_dir.string());
    }
  }

  // Prompt and extract. A wrong pick only costs another trip through the
  // dialog — the already-extracted game files are never at stake here.
  std::string msg =
      "This build of the game requires the official title update to "
      "continue.\n\n"
      "Select the game's title update package file.";

  while (true) {
    const char* const tu_buttons[] = {"Browse...", "Quit"};
    int btn = ShowInfoBox("Title Update Required", msg, tu_buttons);
    if (btn != 0)
      return false;

    std::vector<SDL_DialogFileFilter> tu_filters = {{"All files", "*"}};
    std::string selected;
    if (!RunFileDialog(selected, tu_filters)) {
      // Cancelling the file dialog returns to the prompt, where "Quit" is the
      // way out.
      continue;
    }

    // title_update_sha256 is known non-empty here — an empty one returns above.
    std::string actual = Sha256File(selected);
    if (!HexEqual(actual, settings.title_update_sha256)) {
      REXLOG_ERROR("TU SHA-256 mismatch: expected={}, actual={}", settings.title_update_sha256,
                   actual);
      ShowErrorBox("SHA-256 Mismatch",
                   "The selected title update does not match the expected "
                   "hash.\n\n"
                   "Please select the correct file.");
      continue;
    }

    REXLOG_INFO("Extracting title update from {}...", selected);
    uint32_t tu_count = ExtractTitleUpdateTo(std::filesystem::path(selected), dir, update_dir);
    if (tu_count == 0) {
      ShowErrorBox("Extraction Failed",
                   "Failed to extract the title update.\n\n"
                   "The file may be damaged or not a valid title update "
                   "package.");
      continue;
    }

    if (!std::filesystem::is_regular_file(xexp_path)) {
      ShowErrorBox("Validation Failed", "The title update did not produce a valid default.xexp.");
      continue;
    }

    REXLOG_INFO("Title update extracted: {} files", tu_count);
    // The extracted tree is what gets mounted as update:; record it so the
    // caller persists it alongside game_data_root.
    REXCVAR_SET(update_data_root, update_dir.string());
    return true;
  }
}

// =============================================================================
// GameDataSelector::EnsureGameData
// =============================================================================

namespace {

bool EnsureGameDataImpl(const GameDataSelectorSettings& settings) {
  g_progress_theme = settings.progress_theme;
  g_progress_icon_data = settings.progress_icon_data;
  g_progress_icon_size = settings.progress_icon_size;
  g_progress_callback = settings.progress_callback;

#if REX_PLATFORM_ANDROID
  RemoveLegacyImportCopy();
#endif

  // 1. Check whether game_data_root is already valid.
  std::filesystem::path dir;
  {
    std::string gdr = REXCVAR_GET(game_data_root);
    if (!gdr.empty() && IsGameDataValid(gdr, settings)) {
      REXLOG_INFO("game_data_root already valid: {}", gdr);
      dir = std::filesystem::path(gdr);
      // The game files are known-good, so a failed title update means the user
      // quit out of the TU prompt. Re-prompting for the game files here would
      // throw away a perfectly valid extraction.
      if (!ProcessTitleUpdate(dir, settings)) {
        return false;
      }
      PersistGameDataRoot(settings);
      return true;
    }
  }

  // 1b. No usable game_data_root: look for an extraction left by an earlier run
  // (or dropped in by hand) before asking the user for anything. On Android this
  // is the only way an already-extracted copy can be used at all, since there is
  // no folder picker to point at one.
  for (const auto& candidate : GetPreExtractedCandidates()) {
    if (!IsGameDataValid(candidate.string(), settings)) {
      REXLOG_INFO("No usable game data at {}", candidate.string());
      continue;
    }
    REXLOG_INFO("Found already-extracted game data at {}", candidate.string());
    dir = candidate;
    if (!ProcessTitleUpdate(dir, settings)) {
      return false;
    }
    REXCVAR_SET(game_data_root, dir.string());
    PersistGameDataRoot(settings);
    return true;
  }

  // 2. Inform the user what's needed.
  std::string msg = "This recompilation needs the original game files.\n\n";
  if (settings.is_xbla) {
    msg += "Select an Xbox Live Arcade package (XBLA).";
  } else {
    msg += "Select an Xbox 360 game disc (ISO).";
  }
  msg += "\n\nThe files will be extracted automatically.\n";
  if (kCanPickFolder) {
    msg +=
        "Choose \"Select Folder\" instead to point at an already-extracted "
        "directory containing default.xex.";
  } else {
    // No folder picker here, so the alternative is a path the user copies the
    // extracted files to. Keep it to one short line: SDL's Android message box
    // does not scroll, and a longer message pushes the buttons off screen.
    std::string hint = UserReachableAssetsHint();
    if (!hint.empty()) {
      msg += "Already extracted? Copy them to\n" + hint;
    }
  }

  const char* const buttons_with_folder[] = {"Select File...", "Select Folder...", "Quit"};
  const char* const buttons_file_only[] = {"Select File...", "Quit"};
  const int quit_index = kCanPickFolder ? 2 : 1;
  int btn = kCanPickFolder ? ShowInfoBox("Game Files Required", msg, buttons_with_folder)
                           : ShowInfoBox("Game Files Required", msg, buttons_file_only);
  if (btn < 0 || btn >= quit_index) {
    return false;
  }
  const bool pick_folder = kCanPickFolder && btn == 1;

  // 3. Build the native file-dialog filter list.
  // XBLA content files are extensionless, so "All files" must always be
  // offered — otherwise the user simply can't see the file they need to pick.
  std::vector<SDL_DialogFileFilter> filters;
  if (!settings.is_xbla) {
    filters.push_back({"Xbox 360 Game Disc", "iso"});
  }
  filters.push_back({"All files", "*"});

  // 4. Show the native dialog the user asked for.
  std::string selected;
  bool picked = pick_folder ? RunFolderDialog(selected) : RunFileDialog(selected, filters);
  if (!picked) {
    const char* const ok_button[] = {"OK"};
    ShowInfoBox("Nothing Selected", "No file or folder was selected. The application will exit.",
                ok_button);
    return false;
  }

  std::filesystem::path selected_path(selected);

#if REX_PLATFORM_ANDROID
  // Android's Storage Access Framework returns content:// URIs from the file
  // picker. These can only be read through ContentResolver, not via normal
  // file I/O. FileReader opens them through SDL_IOStream, so the URI is passed
  // straight through to sniffing and extraction: the image is read where it
  // already lives instead of being duplicated into app storage first.
  if (IsContentTreeUri(selected)) {
    // Not reachable through the buttons above (kCanPickFolder is false here),
    // but a tree URI reaching the file branch would otherwise be "copied" into
    // a zero-byte file and reported as a damaged disc image.
    REXLOG_ERROR("Got a SAF tree URI ({}), which cannot be read as a file", selected);
    ShowErrorBox("Unsupported Selection",
                 "Directories picked through the Android file picker cannot be "
                 "read by the app.\n\n"
                 "Select a disc image file instead, or copy already-extracted "
                 "files into the app's storage folder.");
    return false;
  }
  const bool is_content_uri = IsContentUri(selected);
  if (is_content_uri) {
    REXLOG_INFO("Android: extracting in place from content URI {}", selected);
  }
#else
  const bool is_content_uri = false;
#endif

  // Picking default.xex itself inside an extracted directory is a natural
  // mistake — treat it as selecting the directory that contains it. A content
  // URI is never a path std::filesystem can stat, so the query is skipped
  // rather than being answered from a bogus relative path.
  if (!is_content_uri && std::filesystem::is_regular_file(selected_path) &&
      HasExtension(selected_path, "xex") && selected_path.has_parent_path()) {
    REXLOG_INFO("Selected {}, using its parent directory as the game data root",
                selected_path.filename().string());
    selected_path = selected_path.parent_path();
  }

  // 5. Determine destination for extraction.
  std::filesystem::path out_dir = GetWritableBaseDir() / "assets";

  // 6. Process the selection.
  if (!is_content_uri && std::filesystem::is_directory(selected_path)) {
    dir = selected_path;
    if (!ValidateDefaultXexInDir(dir, settings)) {
      ShowErrorBox("Validation Failed",
                   "default.xex was not found or its SHA-256 hash did not "
                   "match.\n\n"
                   "Please make sure the directory contains the extracted "
                   "game files.");
      return false;
    }
  } else {
    // File selection: ISO or XBLA. Identify by content, falling back to the
    // extension only when the file can't be sniffed.
    bool is_xbla = IsStfsPackage(selected_path);
    bool is_iso = !is_xbla && IsXdvdfsImage(selected_path);
    if (!is_iso && !is_xbla) {
      is_iso = HasExtension(selected_path, "iso");
      is_xbla = HasExtension(selected_path, "xbla");
    }

    if (!is_iso && !is_xbla) {
      ShowErrorBox("Unsupported File",
                   "The selected file is neither an Xbox 360 disc image "
                   "(XDVDFS) nor an STFS/XBLA package.\n\n"
                   "Please select a valid Xbox 360 game disc or XBLA "
                   "package.");
      return false;
    }

    REXLOG_INFO("Extracting {} to {}...", selected_path.string(), out_dir.string());

    uint32_t file_count = 0;
    if (is_iso) {
      file_count = ExtractIsoTo(selected_path, out_dir);
    } else {
      file_count = ExtractXblaTo(selected_path, out_dir);
    }

    const char* source_desc = is_iso ? "disc image" : "XBLA package";
    if (file_count == 0) {
      ShowErrorBox("Extraction Failed",
                   std::string("Failed to extract the game files.\n\nThe file may be damaged "
                               "or not a valid Xbox 360 ") +
                       source_desc + ".");
      return false;
    }
    REXLOG_INFO("Extraction complete: {} files written", file_count);

    if (!ValidateDefaultXexInDir(out_dir, settings)) {
      ShowErrorBox("Validation Failed",
                   std::string("default.xex was not found or its SHA-256 hash did not "
                               "match after extraction.\n\nThe ") +
                       source_desc + " may be from a different version of the game.");
      return false;
    }
    dir = out_dir;
  }

  // 8. Handle title update.
  if (!ProcessTitleUpdate(dir, settings)) {
    return false;
  }

  REXCVAR_SET(game_data_root, dir.string());
  REXLOG_INFO("Game data set to: {}", dir.string());

  // Persist the resolved root so the wizard only runs once and an
  // already-extracted directory can be used where it lives, instead of being
  // duplicated next to the executable.
  PersistGameDataRoot(settings);
  return true;
}

}  // namespace

bool GameDataSelector::EnsureGameData(const GameDataSelectorSettings& settings) {
  // This runs before any window or logging UI exists, and it touches the
  // filesystem constantly (create_directories, ofstream, copy). An escaped
  // exception here would terminate the process with no explanation at all, so
  // failures are turned into the same message box every other error path uses.
  try {
    return EnsureGameDataImpl(settings);
  } catch (const std::exception& e) {
    REXLOG_ERROR("GameDataSelector: unhandled exception: {}", e.what());
    ShowErrorBox(
        "Setup Failed",
        std::string("Something went wrong while preparing the game files:\n\n") + e.what());
    return false;
  } catch (...) {
    REXLOG_ERROR("GameDataSelector: unhandled non-standard exception");
    ShowErrorBox("Setup Failed", "Something went wrong while preparing the game files.");
    return false;
  }
}

bool ApplyReleasePatch(std::span<const uint8_t> patch_data, const std::vector<uint8_t>& source,
                       std::vector<uint8_t>& out) {
  const auto patch = ParseXexPatch(patch_data);
  return patch && source.size() == patch->source_size &&
         HexEqual(Sha256Hex(source.data(), source.size()), patch->source_sha256) &&
         ApplyXexPatch(*patch, source, out) &&
         HexEqual(Sha256Hex(out.data(), out.size()), patch->target_sha256);
}

}  // namespace rex::system