# Build instructions

## Windows 10/11 x64 (the real target — untested by the author)
Prereqs: Visual Studio 2022 (MSVC v143, "Desktop development with C++"), CMake ≥ 3.25, Git.
```bat
git clone <this repo> asio-stream-bridge && cd asio-stream-bridge
git clone --depth 1 --recursive --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git third_party\vst3sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release          :: core tests
build\bin\Release\validator.exe build\VST3\Release\ASIOStreamBridge.vst3   :: if the validator target was built
```
The build runs the Steinberg validator automatically after linking (`SMTG_RUN_VST_VALIDATOR=ON`). Static CRT (/MT) is forced for the project and SDK.

Install (admin shell, or per-user location FL Studio also scans):
```bat
xcopy /E /I build\VST3\Release\ASIOStreamBridge.vst3 "C:\Program Files\Common Files\VST3\ASIOStreamBridge.vst3"
```
Then in FL Studio: Options → Manage plugins → Find plugins; add to the **Master** mixer track, last in the chain.
Keep Audio → Device = your Focusrite ASIO driver.

## Linux (CI / dev only — stub sink, no real audio output)
```bash
git clone --depth 1 --recursive --shallow-submodules https://github.com/steinbergmedia/vst3sdk.git third_party/vst3sdk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/core_tests ; ./build/bin/Release/processor_bench
```
Options: `-DASB_BUILD_VST3=OFF` (core only, no SDK needed), `-DASB_BUILD_TESTS=OFF`, `-DASB_VST3_SDK_PATH=<path>`.
