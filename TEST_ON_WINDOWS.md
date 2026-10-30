# Quick Windows test (for the friend)

1. Push this repo to GitHub (private is fine) → Actions tab → `windows-build` → download artifact **ASIOStreamBridge-win64**.
   If the build is red, send back `build-log.txt` (or the log text) — that's the first thing to fix.
2. `asb_probe.exe list` → note your output device's index. `asb_probe.exe <index> 8` → expect a quiet steady 440 Hz tone and
   `status=Streaming`, `underruns` ~0–1, `overflowFrames=0`. Report the printed lines + whether it crackled.
3. Copy `ASIOStreamBridge.vst3` folder to `C:\Program Files\Common Files\VST3\`, rescan in FL Studio (Focusrite ASIO selected),
   add to the **Master** track last. Turn Streaming ON, pick Output Device in the plugin's parameter list (wrench/gear → plugin settings, or the
   generic parameter view). Send back: loads? audio passes? stream audible on chosen device? CPU? crackles? crash on device switch/unplug?
