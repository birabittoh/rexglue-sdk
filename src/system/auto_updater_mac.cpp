/**
 * @file        system/auto_updater_mac.cpp
 * @brief       macOS implementation of AutoUpdater::ApplyAndRestart. See
 *              auto_updater.h.
 *
 * @remarks     Same detached-helper-script shape as auto_updater_posix.cpp,
 *              since a running process cannot swap its own executable and
 *              dylibs out from under itself. macOS adds one constraint the
 *              Linux path does not have: an .app bundle carries a code signature
 *              over its entire contents. Replacing files *inside* the running
 *              bundle invalidates that signature, and the relaunched app is
 *              then killed by Gatekeeper. So when the executable lives in a
 *              bundle, the whole .app is swapped as a unit, which leaves the
 *              incoming bundle's own signature intact.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/auto_updater.h>

#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <rex/logging.h>

namespace rex::system {

namespace {

// If `executable_path` is <something>.app/Contents/MacOS/<exe>, return the
// .app; otherwise an empty path, meaning a plain unbundled binary, which is
// what the mac-arm64 preset produces before packaging.
std::filesystem::path EnclosingAppBundle(const std::filesystem::path& executable_path) {
  auto macos_dir = executable_path.parent_path();
  if (macos_dir.filename() != "MacOS") {
    return {};
  }
  auto contents_dir = macos_dir.parent_path();
  if (contents_dir.filename() != "Contents") {
    return {};
  }
  auto bundle = contents_dir.parent_path();
  if (bundle.extension() != ".app") {
    return {};
  }
  return bundle;
}

// Pick the staged entry that replaces `bundle`. Prefer an exact name match,
// then any single .app, so a release that renames the bundle still applies.
std::filesystem::path FindStagedBundle(const std::vector<std::filesystem::path>& staged_entries,
                                       const std::filesystem::path& bundle) {
  std::filesystem::path only_app;
  size_t app_count = 0;
  for (const auto& entry : staged_entries) {
    if (entry.extension() != ".app") {
      continue;
    }
    if (entry.filename() == bundle.filename()) {
      return entry;
    }
    only_app = entry;
    ++app_count;
  }
  return app_count == 1 ? only_app : std::filesystem::path{};
}

}  // namespace

bool AutoUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& executable_path) {
  if (!HasPendingSelfUpdate(install_root)) {
    return false;
  }
  auto staging = StagingRoot(install_root);

  std::error_code ec;
  std::vector<std::filesystem::path> staged_entries;
  for (auto& entry : std::filesystem::directory_iterator(staging, ec)) {
    staged_entries.push_back(entry.path());
  }
  if (staged_entries.empty()) {
    return false;
  }

  auto bundle = EnclosingAppBundle(executable_path);
  std::filesystem::path staged_bundle;
  if (!bundle.empty()) {
    staged_bundle = FindStagedBundle(staged_entries, bundle);
    if (staged_bundle.empty()) {
      // Running from a bundle but the release does not ship one. Merging the
      // loose files into the bundle would break its signature and get the
      // relaunch killed, so refuse rather than brick the install.
      REXSYS_WARN(
          "AutoUpdater: running from {} but the staged update contains no .app; "
          "refusing to apply (it would invalidate the bundle signature)",
          bundle.string());
      return false;
    }
  }

  auto temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    REXSYS_WARN("AutoUpdater: no usable temp directory ({})", ec.message());
    return false;
  }
  pid_t pid = getpid();
  auto script_path = temp_dir / ("rex_autoupdate_" + std::to_string(pid) + ".sh");
  auto log_path = temp_dir / "rex_autoupdate.log";
  std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
  if (!script) {
    REXSYS_WARN("AutoUpdater: failed to create update helper script");
    return false;
  }
  script << "#!/bin/sh\n";
  script << "REXLOG=\"" << log_path.string() << "\"\n";
  script << "echo \"==== rex auto-update helper, pid " << pid << " ====\" >>\"$REXLOG\"\n";
  script << "date >>\"$REXLOG\"\n";
  // Wait for the old process to actually go away, with the same zombie and
  // timeout handling as the Linux helper (see auto_updater_posix.cpp for why
  // `kill -0` alone is not enough). BSD `ps -o stat= -p` behaves the same here.
  script << "rexwait=0\n";
  script << "while kill -0 " << pid << " 2>/dev/null; do\n";
  script << "  rexstate=$(ps -o stat= -p " << pid << " 2>/dev/null | head -n1)\n";
  script << "  case \"$rexstate\" in *Z*) break;; esac\n";
  script << "  rexwait=$((rexwait+1))\n";
  script << "  if [ \"$rexwait\" -ge 60 ]; then\n";
  script << "    echo \"[FAILED] pid " << pid << " still alive after ${rexwait}s\" >>\"$REXLOG\"\n";
  script << "    exit 1\n";
  script << "  fi\n";
  script << "  sleep 1\n";
  script << "done\n";
  script << "echo \"pid gone after ${rexwait}s, applying\" >>\"$REXLOG\"\n";

  auto emit_swap = [&](const std::filesystem::path& src, const std::filesystem::path& dest) {
    script << "rm -rf \"" << dest.string() << "\" >>\"$REXLOG\" 2>&1\n";
    script << "if mv \"" << src.string() << "\" \"" << dest.string()
           << "\" >>\"$REXLOG\" 2>&1; then\n";
    script << "  echo \"[ok] " << dest.string() << "\" >>\"$REXLOG\"\n";
    script << "else\n";
    script << "  echo \"[FAILED] " << dest.string() << "\" >>\"$REXLOG\"\n";
    script << "fi\n";
    // Clear the quarantine flag if the payload picked one up. The SDK's own
    // downloader does not set it (only LaunchServices does), so this is
    // normally a no-op; it matters when the archive arrived some other way.
    script << "xattr -d -r com.apple.quarantine \"" << dest.string() << "\" >>\"$REXLOG\" 2>&1\n";
  };

  std::filesystem::path relaunch_target;
  if (!bundle.empty()) {
    // Whole-bundle swap: `bundle` is the destination even when the staged
    // .app is named differently, so the relaunch path stays valid.
    emit_swap(staged_bundle, bundle);
    relaunch_target = bundle;
    // Any other staged entries are siblings of the bundle (data dirs, etc.).
    for (const auto& staged_entry : staged_entries) {
      if (staged_entry == staged_bundle) {
        continue;
      }
      emit_swap(staged_entry, install_root / staged_entry.filename());
    }
  } else {
    for (const auto& staged_entry : staged_entries) {
      emit_swap(staged_entry, install_root / staged_entry.filename());
    }
    script << "chmod +x \"" << executable_path.string() << "\" 2>/dev/null\n";
  }
  script << "rmdir \"" << staging.string() << "\" 2>/dev/null\n";
  script << "echo relaunching >>\"$REXLOG\"\n";
  script << "cd \"" << install_root.string() << "\" || exit 1\n";
  if (!relaunch_target.empty()) {
    // `open -n` hands the bundle to LaunchServices, which is what gives the
    // new instance a normal app session (Dock tile, activation, TCC identity);
    // exec'ing Contents/MacOS directly does not.
    script << "open -n \"" << relaunch_target.string() << "\" >>\"$REXLOG\" 2>&1 || \\\n";
    script << "  \"" << executable_path.string() << "\" &\n";
  } else {
    script << "\"" << executable_path.string() << "\" &\n";
  }
  // Safe to unlink while /bin/sh is still executing this file: sh holds an
  // open fd, and the inode outlives the directory entry.
  script << "rm -- \"$0\"\n";
  script.close();
  std::filesystem::permissions(
      script_path,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      ec);

  pid_t child = fork();
  if (child < 0) {
    REXSYS_WARN("AutoUpdater: fork failed for update helper");
    return false;
  }
  if (child == 0) {
    setsid();  // detach into its own session so it survives this process exiting.
    execl("/bin/sh", "sh", script_path.c_str(), static_cast<char*>(nullptr));
    _exit(127);  // execl only returns on failure.
  }
  return true;
}

}  // namespace rex::system
