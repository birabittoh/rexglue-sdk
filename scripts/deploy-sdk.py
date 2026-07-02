#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# ── Config ───────────────────────────────────────────────────────────────────
# Target project directory names (siblings of the SDK root). The SDK is deployed
# into each project's `sdk/` directory. Missing projects are skipped with a
# warning; pass one or more --project NAME flags to select targets.
PROJECTS: list[str] = []

def _detect_preset() -> str:
    if sys.platform == "win32":
        os_name = "win"
    elif sys.platform == "darwin":
        os_name = "mac"
    else:
        os_name = "linux"
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "amd64"
    return f"{os_name}-{arch}"

CONFIGS = ("Debug", "Release", "RelWithDebInfo")

parser = argparse.ArgumentParser(description="Build and deploy the ReXGlue SDK.")
# Each deploy wipes and replaces the target project's sdk/ directory wholesale
# (step 4 below), so deploying Release after a RelWithDebInfo deploy (or vice
# versa) silently leaves a project whose exe/DLLs no longer agree with each
# other (e.g. an exe importing rexruntime.dll with only rexruntimerd.dll on
# disk). Default to RelWithDebInfo since that's what most consuming projects'
# own build scripts default to — override with --config if you specifically
# need a Release or Debug deploy, but then redeploy your project's build too
# so the two stay in sync.
parser.add_argument("--config", choices=CONFIGS, default="RelWithDebInfo",
                    help="CMake build configuration (default: RelWithDebInfo)")
parser.add_argument("--preset", default=_detect_preset(),
                    help="CMake configure preset (default: auto-detected)")
parser.add_argument("--project", action="append", metavar="NAME",
                    help="Target project dir (sibling of the SDK root) to "
                         "deploy into (repeatable)")
parser.add_argument("--android-ndk", default=os.environ.get("ANDROID_NDK_HOME"),
                    help="Path to the Android NDK, required when --preset targets "
                         "android-* (default: $ANDROID_NDK_HOME)")
args = parser.parse_args()

BUILD_CONFIG: str = args.config
PRESET: str = args.preset
TARGET_PROJECTS: list[str] = args.project or PROJECTS
if not TARGET_PROJECTS:
    parser.error("no target projects: pass one or more --project NAME flags")
# ─────────────────────────────────────────────────────────────────────────────

SCRIPT_DIR = Path(__file__).resolve().parent
SDK_ROOT = SCRIPT_DIR.parent

PROJECT_DIRS = [(name, (SDK_ROOT / f"../{name}").resolve()) for name in TARGET_PROJECTS]
existing = [(name, d) for name, d in PROJECT_DIRS if d.is_dir()]
for name, d in PROJECT_DIRS:
    if not d.is_dir():
        print(f"warning: skipping '{name}' — '{d}' does not exist.", file=sys.stderr)
if not existing:
    print("error: none of the target project directories exist: "
          f"{', '.join(name for name, _ in PROJECT_DIRS)}", file=sys.stderr)
    sys.exit(1)

THREADS = os.cpu_count() or 1
INSTALL_PREFIX = SDK_ROOT / "out" / "install" / PRESET

def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)

def resolve_symlink_stubs(root: Path) -> list[Path]:
    """On Windows, git checks out symlinks as plain-text stub files containing the
    target path. Find and replace them with copies of the real file."""
    fixed: list[Path] = []
    for stub in root.rglob("*"):
        if not stub.is_file():
            continue
        try:
            if stub.stat().st_size > 512:
                continue
            content = stub.read_text(encoding="utf-8", errors="ignore").strip()
        except OSError:
            continue
        # A symlink stub is a single line that looks like a relative path
        if "\n" in content or not content.startswith(".."):
            continue
        target = (stub.parent / content).resolve()
        if target.is_file() and target != stub.resolve():
            shutil.copy2(target, stub)
            fixed.append(stub)
    if fixed:
        print(f"==> Resolved {len(fixed)} git symlink stub(s) in {root.relative_to(SDK_ROOT)}")
    return fixed

def restore_symlink_stubs(stubs: list[Path]) -> None:
    """Revert the in-place edits made by resolve_symlink_stubs, so submodules
    under thirdparty/ don't end up with permanently dirty working trees."""
    roots: dict[Path, list[str]] = {}
    for stub in stubs:
        repo_root = stub.parent
        while not (repo_root / ".git").exists():
            repo_root = repo_root.parent
        roots.setdefault(repo_root, []).append(str(stub.relative_to(repo_root)))
    for repo_root, rel_paths in roots.items():
        run("git", "-C", str(repo_root), "checkout", "--", *rel_paths)
    if stubs:
        print(f"==> Restored {len(stubs)} git symlink stub(s) to their checked-out state")

# 1. Init submodules if any are missing
result = subprocess.run(
    ["git", "-C", str(SDK_ROOT), "submodule", "status"],
    capture_output=True, text=True, check=True,
)
if any(line.startswith("-") for line in result.stdout.splitlines()):
    print("==> Initializing missing submodules...")
    run("git", "-C", str(SDK_ROOT), "submodule", "update", "--init", "--recursive")

resolved_stubs = resolve_symlink_stubs(SDK_ROOT / "thirdparty")

# 2. Configure (idempotent) + build
# Windows builds default to D3D12-only (see CMakeLists.txt); CI always adds
# -DREXGLUE_USE_VULKAN=ON for win-amd64 release/nightly artifacts, so match
# that here too, otherwise locally-deployed SDKs silently lack Vulkan support
# that official builds have.
extra_cmake_args: list[str] = []
if PRESET.startswith("win"):
    extra_cmake_args.append("-DREXGLUE_USE_VULKAN=ON")
elif PRESET.startswith("mac"):
    # Matches CI (_build-platform.yaml); the mac-*-base presets don't bake this
    # in themselves, so a local deploy would otherwise float to whatever SDK
    # version macOS the host happens to be running.
    extra_cmake_args.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3")
elif PRESET.startswith("android"):
    # android-arm64-base has no host condition (it's cross-compile-only) but
    # requires an NDK toolchain path; the preset itself can't supply one.
    if not args.android_ndk:
        parser.error(
            "--preset android-* requires --android-ndk (or $ANDROID_NDK_HOME) "
            "to locate the NDK toolchain"
        )
    extra_cmake_args.append(f"-DCMAKE_ANDROID_NDK={args.android_ndk}")

print(f"==> Configuring ({PRESET})...")
run("cmake", "--preset", PRESET, *extra_cmake_args)

# rexglue.exe (the codegen tool) is installed CONFIGURATIONS-restricted to
# Release only (cmake/rexglue_install.cmake) -- a Debug/RelWithDebInfo build
# of it runs an order of magnitude slower, so the SDK deliberately never ships
# one. That means rex::rexglue only ends up with a working IMPORTED_LOCATION
# if a Release install actually runs at some point; skip it and consuming
# projects fail to configure at all ("IMPORTED_LOCATION not set for imported
# target rex::rexglue") no matter which --config they build against. CI never
# hits this because it builds/installs the bare "install" target with no
# --config, which this preset's CMAKE_CROSS_CONFIGS=all/CMAKE_DEFAULT_CONFIGS=all
# turns into a build of every configuration at once. Reproduce that here by
# always doing a Release pass in addition to whatever was asked for.
configs_to_build = [BUILD_CONFIG] if BUILD_CONFIG == "Release" else [BUILD_CONFIG, "Release"]

for config in configs_to_build:
    build_preset = f"{PRESET}-{config.lower()}"
    print(f"==> Building ({config}, -j{THREADS})...")
    run("cmake", "--build", "--preset", build_preset, "--", "-j", str(THREADS))

# 3. Install to staging prefix
for config in configs_to_build:
    print(f"==> Installing ({config}) to {INSTALL_PREFIX}...")
    run("cmake", "--install", str(SDK_ROOT / "out" / "build" / PRESET),
        "--config", config, "--prefix", str(INSTALL_PREFIX))

restore_symlink_stubs(resolved_stubs)

# 4. Wipe old SDK, copy fresh (skip .sdk-version) into each target project
for name, project_dir in existing:
    target = project_dir / "sdk"
    print(f"==> Deploying to {target}...")
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True, exist_ok=True)

    for item in INSTALL_PREFIX.iterdir():
        if item.name == ".sdk-version":
            continue
        dest = target / item.name
        if item.is_dir():
            shutil.copytree(item, dest)
        else:
            shutil.copy2(item, dest)

print(f"==> Done. SDK deployed to: {', '.join(name for name, _ in existing)}")
