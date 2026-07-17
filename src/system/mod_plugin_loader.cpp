/**
 * @file        system/mod_plugin_loader.cpp
 * @brief       Host-side loader for mod code-plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mod_plugin.h>

#include <filesystem>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

namespace {

// Mirrors gpu_plugin_loader.cpp's PluginFileName: mod DLLs follow the SDK's
// per-config postfix convention so a mod built alongside a Debug/RelWithDebInfo
// SDK still resolves the matching binary. Unlike GPU plugins, a mod is also
// allowed to ship only a Release DLL (the common case for distributed mods),
// so callers fall back to the bare name when the postfixed one is missing.
std::string ModFileName(std::string_view stem, std::string_view postfix) {
#if REX_PLATFORM_WIN32
  return fmt::format("{}{}.dll", stem, postfix);
#else
  return fmt::format("lib{}{}.so", stem, postfix);
#endif
}

// Matches the "windows-x64" / "linux-x64" / "linux-arm64" keys mod-build
// tooling (e.g. NocturneRecomp-Mods' scripts/make_mods.py) already writes
// into a mod's `platform` manifest field, so a mod distribution zip can ship
// one `code/<platform>/` subdirectory per platform side by side -- needed in
// particular because linux-x64 and linux-arm64 both build to the same
// lib<stem>.so name and would otherwise collide in a flat code/ directory.
constexpr std::string_view ModPlatformDir() {
#if REX_PLATFORM_WIN32
  return "windows-x64";
#elif REX_PLATFORM_LINUX
#if defined(REX_ARCH_ARM64)
  return "linux-arm64";
#elif defined(REX_ARCH_AMD64)
  return "linux-x64";
#else
  return "";
#endif
#else
  return "";
#endif
}

// Mod plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown, same rationale as GPU plugins.
std::vector<platform::DynamicLibrary>& LoadedModPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IModPlugin> LoadModPlugin(const std::filesystem::path& mod_root,
                                          std::string_view mod_name, std::string_view code_stem,
                                          const ModHostContext& ctx) {
  const std::filesystem::path code_dir = mod_root / "code";

  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }

  // Try <platform>/<file> first (a multi-platform distribution, e.g. one
  // pulled straight from a NocturneRecomp-Mods release zip, ships all
  // platforms' binaries side by side this way and expects the host to pick
  // its own), then fall back to a flat code/<file> layout (a mod built and
  // installed for this host's platform only, the common local-dev case).
  auto resolve = [&](std::string_view postfix) -> std::filesystem::path {
    std::string_view platform_dir = ModPlatformDir();
    if (!platform_dir.empty()) {
      std::filesystem::path platform_path =
          code_dir / platform_dir / ModFileName(code_stem, postfix);
      if (std::filesystem::exists(platform_path)) {
        return platform_path;
      }
    }
    return code_dir / ModFileName(code_stem, postfix);
  };

  std::filesystem::path path = resolve(postfix);
  if (!postfix.empty() && !std::filesystem::exists(path)) {
    // Distributed mods commonly ship a single Release build; fall back to it
    // rather than refusing to load into a Debug/RelWithDebInfo host.
    path = resolve("");
  }
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR("Mod '{}' declares code '{}' but no DLL was found at {}", mod_name, code_stem,
                 path.string());
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("Mod '{}' code plugin failed to load: {}", mod_name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<ModAbiVersionFn>(kModAbiVersionSymbol);
  auto create_fn = library.GetSymbol<ModCreateFn>(kModCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("Mod '{}' code plugin is not a rexglue mod plugin (missing {} / {} exports): {}",
                 mod_name, kModAbiVersionSymbol, kModCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kModPluginAbiVersion) {
    REXSYS_ERROR("Mod '{}' code plugin has ABI version {}, host expects {}: {}", mod_name,
                 plugin_abi, kModPluginAbiVersion, path.string());
    return nullptr;
  }

  ModHostContext info = ctx;
  info.struct_size = sizeof(ModHostContext);

  IModPlugin* plugin = create_fn(kModPluginAbiVersion, &info);
  if (!plugin) {
    REXSYS_ERROR("Mod '{}' code plugin factory returned no plugin", mod_name);
    return nullptr;
  }

  LoadedModPlugins().push_back(std::move(library));
  REXSYS_INFO("Mod code plugin '{}' loaded ({})", mod_name, path.filename().string());
  return std::unique_ptr<IModPlugin>(plugin);
}

}  // namespace rex::system
