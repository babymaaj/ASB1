#!/usr/bin/env bash
# Compile-check (and optionally run under Wine) the Windows build from Linux, no Windows PC needed.
# This is a CHECK build with MinGW. The real release build is MSVC (see BUILD_INSTRUCTIONS.md / CI).
#   apt install g++-mingw-w64-x86-64-posix          (compiler)
#   apt install wine64 pulseaudio                   (optional: run the .exe files)
# Two small MinGW-only workarounds are applied to your LOCAL third_party/vst3sdk copy (git-ignored);
# MSVC never takes these code paths.
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=third_party/vst3sdk
A=$SDK/public.sdk/source/vst/utility/alignedalloc.h
grep -q __MINGW32__ "$A" || sed -i 's/^#elif defined(_MSC_VER)$/#elif defined(_MSC_VER) || defined(__MINGW32__)/; s/^#if defined(_MSC_VER)$/#if defined(_MSC_VER) || defined(__MINGW32__)/' "$A"
V=$SDK/public.sdk/samples/vst-hosting/validator/CMakeLists.txt
grep -q municode "$V" || printf 'if(MINGW)\n  target_link_options(validator PRIVATE -municode)\nendif()\n' >> "$V"

cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release -DASB_RUN_VALIDATOR=OFF
# The SDK's post-build step calls Windows' `attrib`, which fails on Linux after the DLL is already linked; -k 0 keeps going.
cmake --build build-win -j"$(nproc)" -- -k 0 || true
cmake --build build-win --target validator
ls -la build-win/VST3/Release/ASB.vst3/Contents/x86_64-win/ASB.vst3

if command -v /usr/lib/wine/wine64 >/dev/null; then
  export WINEDEBUG=-all
  W=/usr/lib/wine/wine64
  $W build-win/core_tests.exe | tail -1
  $W build-win/bin/state_tests.exe | tail -1
  $W build-win/bin/validator.exe build-win/VST3/Release/ASB.vst3 2>&1 | grep Result
  echo "For real audio under Wine: start pulseaudio with a null sink, then: $W build-win/asb_probe.exe list"
fi
