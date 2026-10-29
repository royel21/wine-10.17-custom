# Handoff: Wine 10.17-custom + Winlator work (2026-10-29)

Paste or open this file in a new Claude Code session to continue.

## Repos and paths (WSL, Ubuntu 24.04)
- Wine tree: `/home/$pcuser/wine-10.17-custom` (branch `Old-Reparse`), built with `build.sh` -> `/tmp/wine_build-current` (x86_64 Wine, run under box64 in Winlator).
- Winlator fork: `/mnt/c/Users/$pcuser/AndroidStudioProjects/winlator-rc` (package `com.winlator`, glibc rootfs).
- box64 clone: `/home/$pcuser/box64` (+ `build-winlator.sh`).
- Banners-Turnip clone: `/home/$pcuser/Banners-Turnip` (+ `build_turnip_winlator.sh`).
- Test devices: Snapdragon 8 Gen 3 and 8 Elite (Samsung SM-S936U1).

## Winlator rootfs constraints (verified)
- aarch64 glibc **2.39**, Arch-style `/usr/lib`, no `libwayland-client`.
- Native binaries need interpreter + RUNPATH `/data/data/com.winlator/files/rootfs/lib`.
- `WINEDEBUG` typed in env vars is ignored: Winlator builds it from
  Settings -> Enable Wine debug -> Wine Debug Channel (XServerDisplayActivity.java:633).
- Android/bionic Turnip builds only work as Vortek adrenotools drivers, not as TURNIP `.tzst`.

## CURRENT TASK: game installer (`X:\SETUP.EXE` + `scsetup.dll`, "Oasys system manager")
History:
1. Froze with `loader_section ... blocked by <main thread>`: `scsetup.dll` DllMain calls
   `waveOutGetDevCapsA` / `midiOutGetDevCapsW` under the loader lock.
2. **Fixed (committed: `c9967bf6 some fix`; ddraw front-buffer -> back-buffer+present video fix is `ecda5759`):**
   - `dlls/mmdevapi/main.c`: MIDI `DriverProc` `DRV_FREE` no longer waits for the notify thread
     when the loader lock is held (the thread can't have started -> deadlock). Notify thread now holds
     a module ref (`GetModuleHandleExW` / `FreeLibraryAndExitThread`). **This was the real hang.**
   - `dlls/winmm/waveform.c`: `WINMM_StartDevicesThread` returns FALSE (-> MMSYSERR_NODRIVER) instead
     of deadlocking when called with the loader lock held. Defensive; did not trigger in the last log.
   - Both built OK and are installed on the phone; installer no longer hangs.
3. **Open now:** installer shows "WAVEデバイスが開けません" (can't open WAVE device), repeating ~every 2 s.
   Log: `waveOutOpen` from a normal thread (no loader lock), then
   `warn:winmm:WINMM_OpenDevice Initialize failed: 8889000f` = `AUDCLNT_E_ENDPOINT_CREATE_FAILED`
   -> one of the ALSA steps in `dlls/winealsa.drv/alsa.c` ~820-950 (snd_pcm_open / hw_params / sw_params) fails.
   **Next step:** run with debug channels `alsa,winmm` (+ `mmdevapi`) and read the
   `warn:alsa:... Unable to ...` line. Also ask: does sound work in other games in the same container?

Unrelated, seen in the same logs (later):
- `winebus.sys` page fault (write to 0x20072116E at 0x7FFFAA23B0) — likely the 8BitDo controller
  `WINEBUS\VID_2DC8&PID_FFFF`. Needs `hid,winebus` channels with the controller connected.

## Done this session (Winlator fork, uncommitted)
- GL context-loss recovery (black screen after background): `onContextLost()` in Texture, GPUImage,
  RenderTarget, VertexAttribute, ShaderMaterial (uniforms track program), EffectComposer,
  DrawableManager, CursorManager; hooked in `GLRenderer.onSurfaceCreated` (logs "EGL context was recreated").
- `AndroidManifest.xml`: XServerDisplayActivity configChanges += `uiMode|fontScale|locale|layoutDirection|touchscreen|colorMode|mcc|mnc`
  (activity recreation killed Wine).
- `GeneralComponents.java`: box64 list + `0.4.5`; Turnip list: removed `26.1.15`, added `27.0.0`;
  adrenotools: builtin-driver extraction helper `getBuiltinAdrenotoolsDriverDir` (no bundled drivers now,
  list is `System` only), manifest lookup scans all `.json` for `libraryName`.
- Assets: added `box64/box64-0.4.5.tzst`, `graphics_driver/turnip-27.0.0.tzst` (Mesa 27, X11-only,
  api_version fixed to 1.4.366 — user confirmed it works); removed `turnip-26.1.15.tzst`,
  `adrenotools/Turnip-v26.3.0.tzst`.
- `DrawRequests.copyArea` GPUImage fast path and `ActiveWindowsDialog` `WinTree` debug dump: user
  reverted/changed these on disk; check current state before relying on them.
- Defaults unchanged: `DefaultVersion.BOX64 = 0.4.0`, `DefaultVersion.TURNIP = 26.2.0`
  (offered to bump to 0.4.5 / 27.0.0 after testing).

## Other findings (no code change yet)
- Japanese setup: `LC_ALL=ja_JP.utf8` + `TZ=Asia/Tokyo` already default; rootfs has ja_JP locale and
  msgothic.ttc. `.reg` files are NOT auto-imported; `WineUtils.setSystemFont` wipes WindowMetrics fonts on
  every settings save. Proposed: write FontSubstitutes / Fonts\Replacements in ContainerManager.
- arm64ec: Winlator supports it (bundled Hangover 10.14). A fresh upstream arm64ec Wine needs
  `HKLM\Software\Microsoft\Wow64\amd64`=libarm64ecfex.dll and `\x86`=libwow64fex.dll/wowbox64.dll
  (template has xtajit*). The FEX version picker is never applied (FEXCore/*.tzst unused).
- ntsync-android: feasible only with an upstream-ntsync port + shim; main risk is box64 with
  process-shared robust mutexes (fine on arm64ec).
