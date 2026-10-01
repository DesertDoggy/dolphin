#!/usr/bin/env bash
set -euo pipefail

# Builds dolphinrvz (the ISO/GCZ/WIA/RVZ converter shared library -- see user/dolphinrvz.h)
# for one platform/arch target. Configures dolphin/ itself as the CMake source root, with
# the dolphinrvz target spliced in via -DCMAKE_PROJECT_INCLUDE (see
# dolphinrvz_project_include.cmake for why), never modifying dolphin/'s own build files.
#
# All build output lives under this submodule's own user/ directory -- nothing is ever
# written into the dolphin/ repo tree itself.
#
# Usage:
#   user/scripts/build_dolphin_rvz.sh                    # auto-detect host platform/arch
#   user/scripts/build_dolphin_rvz.sh <platform> <arch>
#   user/scripts/build_dolphin_rvz.sh --nostreaming [<platform> <arch>]
#
# Builds the dolphinrvz_streaming target by default (--nostreaming builds the plain
# dolphinrvz target; --streaming is still accepted, as a no-op). The streaming target is the
# same library plus user/dolphinrvz_stream.cpp (dolphinrvz_extract_stream,
# dolphinrvz_convert_stream and the dolphinrvz_reader_* API -- see dolphinrvz.h), installed
# to user/release/with_streaming/<platform>/<arch>/<version>/. It shares this build tree with
# the plain target (both link the same Dolphin static libraries), so it doesn't rebuild
# Dolphin, and the default target's sources, flags, exports and output path are unchanged.
#
# Targets:
#   windows/x64   linux/x64   mac/arm64   android/arm64   ios/arm64 (experimental)
#
# Prerequisites: see user/README.dolphinrvz.md.

streaming=1
args=()
for arg in "$@"; do
  case "$arg" in
    --streaming) streaming=1 ;;
    --nostreaming) streaming=0 ;;
    *) args+=("$arg") ;;
  esac
done
set -- ${args[@]+"${args[@]}"}

if (( streaming )); then
  cmake_target="dolphinrvz_streaming"
  variant_dir="with_streaming/"
else
  cmake_target="dolphinrvz"
  variant_dir=""
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
user_dir="$(cd "$script_dir/.." && pwd)"
submodule_root="$(cd "$user_dir/.." && pwd)"

if [[ $# -eq 0 ]]; then
  os_name="$(uname -s)"
  cpu_name="$(uname -m)"
  case "$os_name" in
    Darwin) platform="mac" ;;
    Linux) platform="linux" ;;
    MINGW*|MSYS*|CYGWIN*) platform="windows" ;;
    *) echo "[ERROR] Unsupported OS for auto-detect: $os_name" >&2; exit 2 ;;
  esac
  case "$cpu_name" in
    arm64|aarch64) arch="arm64" ;;
    x86_64|amd64) arch="x64" ;;
    *) echo "[ERROR] Unsupported arch for auto-detect: $cpu_name" >&2; exit 2 ;;
  esac
elif [[ $# -eq 2 ]]; then
  platform="$1"
  arch="$2"
else
  echo "[ERROR] Usage: $0 [--nostreaming] OR $0 [--nostreaming] <platform> <arch>" >&2
  exit 2
fi

case "$platform/$arch" in
  windows/x64|windows/arm64|linux/x64|linux/arm64|mac/arm64|android/arm64|ios/arm64) ;;
  *)
    echo "[ERROR] Unsupported platform/arch combination: $platform/$arch" >&2
    echo "[ERROR] Supported: windows/x64 windows/arm64 linux/x64 linux/arm64 mac/arm64 android/arm64 ios/arm64" >&2
    exit 2
    ;;
esac

version="$(git -C "$submodule_root" describe --tags 2>/dev/null || git -C "$submodule_root" rev-parse --short HEAD 2>/dev/null || echo dev)"

log_dir="$user_dir/logs"
mkdir -p "$log_dir"
log_file="$log_dir/build-dolphinrvz-$platform-$arch-$(date +%Y%m%d-%H%M%S).log"

echo "[INFO] platform=$platform arch=$arch version=$version streaming=$streaming" | tee -a "$log_file"

build_dir="$user_dir/_build/$platform/$arch"
out_dir="$user_dir/release/${variant_dir}$platform/$arch/$version/dynamic"
include_dir="$user_dir/release/${variant_dir}$platform/$arch/$version/include"
mkdir -p "$build_dir" "$out_dir" "$include_dir"

# -S points at dolphin/ itself (not user/): dolphin's own CMake code assumes it is always
# the top-level project (e.g. Source/Core/Core/CMakeLists.txt references
# ${CMAKE_SOURCE_DIR}/Data/Sys directly for a generated-header rule) -- CMAKE_SOURCE_DIR
# must stay dolphin/, not user/, or those references break. The dolphinrvz target is
# spliced in via CMAKE_PROJECT_INCLUDE instead (see dolphinrvz_project_include.cmake for
# why a plain add_subdirectory(user) from a wrapper project doesn't work here).
cmake_args=(
  -S "$submodule_root"
  -B "$build_dir"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_PROJECT_INCLUDE="$user_dir/dolphinrvz_project_include.cmake"
  -DDOLPHINRVZ_USER_DIR="$user_dir"
  # dolphinrvz links core/discio/uicommon etc as static libs into itself, a SHARED
  # library (see dolphinrvz_target.cmake) -- unlike Dolphin's own normal build, where
  # those static libs only ever feed the dolphin-emu executable and never need to be
  # position-independent. Without this, thread_local statics (e.g. Core.cpp's
  # tls_is_gpu_thread) get compiled with the "local-exec" TLS model, which ld then
  # rejects when linking into a shared object (confirmed directly: "relocation
  # R_X86_64_TPOFF32 ... can not be used when making a shared object"). Harmless on
  # MSVC/Windows, which ignores this CMake variable.
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

# Locates the Visual Studio install root. Dolphin's own build is only ever tested
# against MSVC upstream (its CMakePresets.json has no MinGW generator, and its CMake has
# real MSVC-only assumptions scattered through it -- e.g. WIL in UICommon, and
# Externals/libusb/CMakeLists.txt's `if(WIN32)` branch, which unconditionally includes an
# MSVC-specific config.h and hard #errors under any other compiler) -- so dolphinrvz
# targets MSVC on Windows even though chdman-simd (a separate, MinGW-first codebase) uses
# plain MSYS2 MinGW64.
find_vs_root() {
  local vs_root="${VS_INSTALL_DIR:-C:\\Visual Studio\\18\\Community}"

  if [[ ! -f "$(cygpath -u "$vs_root\\VC\\Auxiliary\\Build\\vcvarsall.bat")" ]]; then
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -f "$vswhere" ]]; then
      local found_root
      found_root="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>/dev/null | tr -d '\r')"
      [[ -n "$found_root" ]] && vs_root="$found_root"
    fi
  fi

  if [[ ! -f "$(cygpath -u "$vs_root\\VC\\Auxiliary\\Build\\vcvarsall.bat")" ]]; then
    echo "[ERROR] Visual Studio install not found. Set VS_INSTALL_DIR to your install root," >&2
    echo "[ERROR] e.g. VS_INSTALL_DIR='C:\\Visual Studio\\18\\Community'" >&2
    exit 3
  fi

  echo "$vs_root"
}

# Finds a Git executable NOT installed via MSYS2/mingw64 (e.g. a standalone Git for
# Windows install), returning its directory (Windows-style) or empty if none found.
# Only used to keep this build's PATH free of MSYS2 entirely -- see the windows case
# block below for why. Git is optional for the build (Dolphin's version-string
# generation degrades to empty strings if it's unavailable); this is best-effort.
find_native_git_dir() {
  local candidate
  for candidate in \
    "/c/Program Files/Git/cmd" \
    "$HOME/AppData/Local/Programs/Git/cmd" \
    "/c/Users/$(whoami)/AppData/Local/Programs/Git/cmd"
  do
    if [[ -f "$candidate/git.exe" ]]; then
      cygpath -w "$candidate"
      return
    fi
  done
}

case "$platform" in
  windows)
    # cmake_args is not used for windows -- the configure+build block below constructs
    # the equivalent cmake invocation directly as native Windows paths inside a batch
    # file, using Visual Studio's own bundled cmake.exe/ninja.exe rather than MSYS2's.
    vs_root="$(find_vs_root)"
    vcvarsall="$vs_root\\VC\\Auxiliary\\Build\\vcvarsall.bat"
    vs_cmake_dir="$vs_root\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin"
    vs_ninja_dir="$vs_root\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja"
    vs_cmake_exe="$vs_cmake_dir\\cmake.exe"
    vs_ninja_exe="$vs_ninja_dir\\ninja.exe"
    [[ -f "$(cygpath -u "$vs_cmake_exe")" ]] || { echo "[ERROR] VS-bundled cmake.exe not found: $vs_cmake_exe (needs the \"C++ CMake tools for Windows\" component)" >&2; exit 3; }
    [[ -f "$(cygpath -u "$vs_ninja_exe")" ]] || { echo "[ERROR] VS-bundled ninja.exe not found: $vs_ninja_exe (needs the \"C++ CMake tools for Windows\" component)" >&2; exit 3; }
    native_git_dir="$(find_native_git_dir)"
    echo "[INFO] Using MSVC via: $vcvarsall" | tee -a "$log_file"
    echo "[INFO] Using VS-bundled cmake: $vs_cmake_exe" | tee -a "$log_file"
    echo "[INFO] Using VS-bundled ninja: $vs_ninja_exe" | tee -a "$log_file"
    [[ -n "$native_git_dir" ]] && echo "[INFO] Using native git: $native_git_dir\\git.exe" | tee -a "$log_file"
    # arm64: cross from an x64 host via vcvarsall x64_arm64, native on an arm64 one. The
    # explicit CMAKE_SYSTEM_PROCESSOR matters -- Dolphin picks its JIT/codegen and SIMD
    # paths from it, and without CMAKE_SYSTEM_NAME CMake reports the *host's* AMD64.
    win_arch_defs=""
    if [[ "$arch" == "arm64" ]]; then
      if [[ "${PROCESSOR_ARCHITECTURE:-}" == "ARM64" ]]; then vcvars_arch="arm64"; else vcvars_arch="x64_arm64"; fi
      win_arch_defs=" -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64"
    else
      vcvars_arch="x64"
    fi
    echo "[INFO] vcvarsall arch: $vcvars_arch" | tee -a "$log_file"
    ;;
  linux)
    cmake_args+=(-G Ninja)
    # linux/arm64 from a non-arm64 host: the GNU aarch64 cross compilers (apt:
    # g++-aarch64-linux-gnu), find root on their sysroot so no x86_64 host library leaks in.
    # Dolphin bundles every dependency dolphinrvz needs under Externals/, so nothing else
    # has to exist for arm64. LINUX_ARM64_TOOLCHAIN_FILE overrides all of it.
    if [[ "$arch" == "arm64" ]]; then
      case "$(uname -m)" in arm64|aarch64) host_is_arm64=1 ;; *) host_is_arm64=0 ;; esac
      if [[ -n "${LINUX_ARM64_TOOLCHAIN_FILE:-}" ]]; then
        cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$LINUX_ARM64_TOOLCHAIN_FILE")
      elif (( ! host_is_arm64 )) || [[ -n "${LINUX_ARM64_CROSS_PREFIX:-}" ]]; then
        prefix="${LINUX_ARM64_CROSS_PREFIX:-aarch64-linux-gnu-}"
        command -v "${prefix}g++" >/dev/null 2>&1 || { echo "[ERROR] ${prefix}g++ not found. Install g++-aarch64-linux-gnu (or set LINUX_ARM64_TOOLCHAIN_FILE)." >&2; exit 3; }
        cmake_args+=(
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
          -DCMAKE_C_COMPILER="${prefix}gcc" -DCMAKE_CXX_COMPILER="${prefix}g++"
          -DCMAKE_FIND_ROOT_PATH="/usr/${prefix%-}"
          -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
          -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
        )
      fi
    fi
    ;;
  mac)
    cmake_args+=(-G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
    # No SDL at all. It is only Dolphin's generic controller backend, which a disc-image
    # library never touches, and both ways of keeping it are wrong here: left on, a Homebrew
    # sdl3 that happens to be installed gets linked by absolute path
    # (/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib), so the library loads only on machines
    # that also have it; forced to the bundled static one (USE_SYSTEM_SDL3=OFF), its own
    # copy of hidapi collides with Externals/hidapi -- "ld: 4 duplicate symbols"
    # (_hid_darwin_*) when everything is linked into one dylib.
    cmake_args+=(-DENABLE_SDL=OFF)
    ;;
  android)
    : "${ANDROID_NDK_HOME:?[ERROR] ANDROID_NDK_HOME must point to an installed Android NDK (r26+)}"
    cmake_args+=(
      -G Ninja
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
      -DANDROID_ABI=arm64-v8a
      -DANDROID_PLATFORM=android-30
      -DANDROID=1
    )
    ;;
  ios)
    command -v xcrun >/dev/null 2>&1 || { echo "[ERROR] xcrun not found (requires Xcode)" >&2; exit 3; }
    echo "[WARN] iOS is experimental -- Dolphin upstream has no official iOS target." | tee -a "$log_file"
    echo "[WARN] discio/core/uicommon may need real fixes here that don't exist yet" | tee -a "$log_file"
    echo "[WARN] upstream; expect to iterate." | tee -a "$log_file"
    cmake_args+=(
      -G Ninja
      -DCMAKE_SYSTEM_NAME=iOS
      -DCMAKE_OSX_ARCHITECTURES=arm64
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
      -DCMAKE_OSX_SYSROOT=iphoneos
    )
    ;;
esac

jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [[ "$platform" == "windows" ]]; then
  # Configure and build both run inside ONE batch file (after `call vcvarsall.bat`)
  # instead of being invoked from bash directly, for a reason confirmed by direct
  # testing, not guesswork: importing the MSVC environment into bash (via `export`) and
  # then invoking cmake from bash works fine for the configure step itself, but several
  # process-hops further down the chain bash -> cmake.exe -> ninja.exe -> cmd.exe ->
  # link.exe (ninja wraps each MSVC link rule in its own cmd.exe /C "...") the
  # bash-exported INCLUDE/LIB/LIBPATH stop reaching link.exe, which then fails to find
  # even kernel32.lib. Running cmake itself as a direct child of the SAME vcvars-configured
  # cmd.exe process (rather than crossing back through bash's environment) avoids that
  # entirely -- confirmed working by reproducing a full compile+link this way directly.
  #
  # Paths are converted to native Windows form (cygpath -w) since, unlike bash invoking
  # cmake.exe directly (where MSYS2 auto-translates plain path-like arguments), a batch
  # script's own content gets no such translation.
  win_submodule_root="$(cygpath -w "$submodule_root")"
  win_build_dir="$(cygpath -w "$build_dir")"
  win_project_include="$(cygpath -w "$user_dir/dolphinrvz_project_include.cmake")"
  win_user_dir="$(cygpath -w "$user_dir")"

  tmp_bat="$(mktemp --suffix=.bat)"
  win_tmp_bat="$(cygpath -w "$tmp_bat")"
  {
    echo "@echo off"
    # PATH is reset to a clean, MSYS2-free base BEFORE calling vcvarsall.bat, so its own
    # additions (cl.exe, link.exe, Windows SDK tools) prepend onto this clean base rather
    # than onto bash's inherited PATH (which has C:\msys64\... all over it). This is
    # necessary, not just tidy: with msys64 on PATH, CMake's find_package machinery kept
    # discovering MinGW-targeted "system" zlib/zstd/bzip2/lzma/etc (via pkg-config.exe,
    # itself only reachable from mingw64/bin) instead of falling back to Dolphin's own
    # bundled Externals/ copies. Those MinGW builds are incompatible with an MSVC link,
    # and their headers use GCC-specific syntax cl.exe can't parse -- confirmed directly:
    # this broke minizip-ng's build on __UINTPTR_TYPE__/__asm__ from MinGW's own
    # corecrt.h/stdlib.h, reached via an -I flag CMake added after finding it through this
    # exact mechanism. (CMake's find_path()/find_library() also derive "<dir>/../include"
    # from every bin/ dir on PATH by default, independent of pkg-config -- confirmed
    # neutering pkg-config alone doesn't stop it either.) A clean PATH avoids the whole
    # class of problem at the source instead of chasing each dependency that hits it.
    echo "set \"PATH=C:\\Windows\\System32;C:\\Windows;C:\\Windows\\System32\\Wbem;C:\\Windows\\System32\\WindowsPowerShell\\v1.0;C:\\Windows\\System32\\OpenSSH;$vs_cmake_dir;$vs_ninja_dir${native_git_dir:+;$native_git_dir}\""
    echo "call \"$vcvarsall\" $vcvars_arch >nul 2>&1"
    echo "echo [INFO] Configuring ..."
    echo "\"$vs_cmake_exe\" -S \"$win_submodule_root\" -B \"$win_build_dir\" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_INCLUDE=\"$win_project_include\" -DDOLPHINRVZ_USER_DIR=\"$win_user_dir\" -G Ninja -DCMAKE_MAKE_PROGRAM=\"$vs_ninja_exe\" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl$win_arch_defs"
    echo "if errorlevel 1 exit /b 1"
    echo "echo [INFO] Building $cmake_target ..."
    echo "\"$vs_cmake_exe\" --build \"$win_build_dir\" --target $cmake_target --config Release -j$jobs"
  } > "$tmp_bat"

  # MSYS2_ARG_CONV_EXCL="/c": without it, MSYS2's argv-to-Windows path translation sees
  # the literal token "/c" and mangles it into "C:\" (since /c is MSYS2's own
  # drive-mount notation for the C: drive) before cmd.exe ever sees it, so cmd.exe gets
  # no recognizable /c switch and just starts interactively instead of running the batch
  # file. < /dev/null: cmd.exe launched from bash under MSYS2's mintty console doesn't
  # get a properly connected stdin, and will hang waiting for input that never arrives
  # if it (or anything the batch file chains into) ever probes for it. Both confirmed by
  # direct testing, not guesswork.
  set +e
  MSYS2_ARG_CONV_EXCL="/c" cmd.exe /c "$win_tmp_bat" < /dev/null 2>&1 | tee -a "$log_file"
  build_status="${PIPESTATUS[0]}"
  set -e
  rm -f "$tmp_bat"
  [[ "$build_status" -eq 0 ]] || { echo "[ERROR] Windows configure/build failed (exit $build_status)" | tee -a "$log_file"; exit "$build_status"; }
else
  echo "[INFO] Configuring ..." | tee -a "$log_file"
  cmake "${cmake_args[@]}" 2>&1 | tee -a "$log_file"

  echo "[INFO] Building $cmake_target ..." | tee -a "$log_file"
  cmake --build "$build_dir" --target "$cmake_target" --config Release -j"$jobs" 2>&1 | tee -a "$log_file"
fi

case "$platform" in
  windows) built_name="dolphinrvz.dll" ;;
  mac|ios) built_name="libdolphinrvz.dylib" ;;
  *) built_name="libdolphinrvz.so" ;;
esac

# Both targets produce the same file name: the streaming one only ever lives under
# with_streaming/ (see dolphinrvz_target.cmake), and the default one never does.
if (( streaming )); then
  built_path="$(find "$build_dir/with_streaming" -iname "$built_name" -print -quit 2>/dev/null)"
else
  built_path="$(find "$build_dir" -path "$build_dir/with_streaming" -prune -o -iname "$built_name" -print -quit 2>/dev/null)"
fi
if [[ -z "$built_path" ]]; then
  echo "[ERROR] Build did not produce $built_name under $build_dir" | tee -a "$log_file"
  exit 5
fi

cp -f "$built_path" "$out_dir/$built_name"
cp -f "$user_dir/dolphinrvz.h" "$include_dir/dolphinrvz.h"

echo "" | tee -a "$log_file"
echo "[INFO] Build complete." | tee -a "$log_file"
echo "[INFO] Library : $out_dir/$built_name" | tee -a "$log_file"
echo "[INFO] Header  : $include_dir/dolphinrvz.h" | tee -a "$log_file"
