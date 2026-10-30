# Build instructions

## Windows 10/11 x64 (the real target — untested by the author)
Prereqs: Visual Studio 2022 or 2026 ("Desktop development with C++"), CMake ≥ 3.25 (≥ 4.2 for VS 2026), Git.
```bat
git clone <this repo> asio-stream-bridge && cd asio-stream-bridge
git clone --depth 1 --recursive --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git third_party\vst3sdk
cmake -S . -B build -A x64          :: picks your installed Visual Studio
cmake --build build --config Release
ctest --test-dir build -C Release          :: core + state tests
build\bin\Release\validator.exe build\VST3\Release\ASB.vst3   :: if the validator target was built
```
The build runs the Steinberg validator automatically after linking (`SMTG_RUN_VST_VALIDATOR=ON`). Static CRT (/MT) is forced for the project and SDK.

Install (admin shell, or per-user location FL Studio also scans):
```bat
xcopy /E /I build\VST3\Release\ASB.vst3 "C:\Program Files\Common Files\VST3\ASB.vst3"
```
Then in FL Studio: Options → Manage plugins → Find plugins; add to the **Master** mixer track, last in the chain.
Keep Audio → Device = your Focusrite ASIO driver.

## Linux (CI / dev only — stub sink, no real audio output)
```bash
git clone --depth 1 --recursive --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git third_party/vst3sdk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/core_tests ; ./build/bin/Release/processor_bench
```
Options: `-DASB_BUILD_VST3=OFF` (core only, no SDK needed), `-DASB_BUILD_TESTS=OFF`, `-DASB_VST3_SDK_PATH=<path>`,
`-DASB_RUN_VALIDATOR=OFF` (skip the post-link validator, e.g. cross-builds).

## Windows build from Linux (check build: MinGW + optional Wine)
```bash
sudo apt install g++-mingw-w64-x86-64-posix   # + wine64 pulseaudio to run the .exe files
./tools/cross_check_windows.sh
```
Compiles the Windows engine/plugin/tests with MinGW and, if Wine is installed, runs `core_tests.exe`, `state_tests.exe` and
the Windows validator on the Windows DLL. Applies two MinGW-only tweaks to your local (git-ignored) SDK copy. Release builds
should still be MSVC.
