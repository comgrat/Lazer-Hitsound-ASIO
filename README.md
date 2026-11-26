# Lazer Hitsound ASIO

Standalone ASIO output for gameplay hitsounds on reviewed osu!lazer / Tachyon audio implementations. Music and UI effects keep the game's native output.

**[Download the Windows x64 portable release](https://github.com/comgrat/Lazer-Hitsound-ASIO/releases/latest)** · [五語使用說明 / Five-language guide](docs/USAGE.md) · [Build](docs/BUILD.md) · [Verification](docs/VERIFICATION.md)

本项目基于 [Mother-Ship/osu-lazer-External-ASIO-Sound](https://github.com/Mother-Ship/osu-lazer-External-ASIO-Sound) 修改；更早上游为 [XTXTMTXTX/osu-External-ASIO-Sound](https://github.com/XTXTMTXTX/osu-External-ASIO-Sound)。由 comgrat 独立维护，非上游官方发布。保留原作者署名和提交历史。

This fork is based on Mother-Ship's lazer adaptation and XTXTMTXTX's original project. It is independently maintained by **comgrat** and is not an official upstream release. See [AUTHORS](AUTHORS.md) and [third-party notices](THIRD_PARTY_NOTICES.md). **No project-wide license has been established**; [LICENSE](LICENSE) records the unresolved upstream and FMOD permissions.

## 使用 / Quick start

1. 从上面的 **Releases** 下载 ZIP 并完整解压；GitHub 的“Source code”压缩包是源码，不是可运行程序。
2. 打开 `LazerASIO.exe`，按初始设置向导选择语言、自己的 ASIO 设备、缓冲与采样率和音量。
3. 主界面显示“等待 osu!lazer”时照常启动游戏（按游戏文件识别，EXE 改名也无需设置）；状态变为“已附加就绪”后进入谱面。
4. 音量条拖动即实时生效；语言可在右上角随时切换。关闭窗口时可最小化到托盘继续运行。

Download the portable ZIP, extract the entire folder, and open `LazerASIO.exe`. The setup wizard asks for your language, ASIO device, buffer/sample rate and volume. While the window shows "Waiting for osu!lazer", launch the game normally (it is recognized by its files, under any EXE name) and enter a beatmap once it reads "Attached and ready". The volume slider applies live while attached, and the language menu switches the interface immediately.

Five languages are integrated into the same application: **简体中文 · 繁體中文 · English · Français · Español**.

## Behavior

- Routes identified gameplay hitsounds through the external FMOD ASIO backend, suppressing their corresponding native channels after successful takeover. BGM and UI effects remain native.
- On the reviewed implementations, the in-game Hitsound slider at **0% still allows ASIO hitsounds**. Metadata inspection of Game2026.1115 confirms that HitsoundVolume binds GameplayMixer.Volume, which the bridge excludes from the external gain; the binding and setting value are checked by the contract. This does not establish every future client's slider behavior. Use the volume slider in `LazerASIO.exe` to control ASIO volume; while attached it applies as you drag. Editing `Output Volume` in `config.ini` still works and applies about 0.5 seconds after saving. **0 mutes ASIO**.
- Preserves beatmap/sample gain, pan, playback rate and legitimate layered hitsounds. Layered sounds are not deduplicated by sample name or a time window.
- Detects normally launched clients without copying or starting the game. Exiting the game releases the ASIO device, and the tool keeps waiting for the next client.
- Supports renamed outer tool folders and paths containing spaces or Chinese text. Keep the internal file names, `managed` and `locales` folders intact.
- Diagnostic file/console writes run in the background so a slow log sink cannot serialize hit layers. Debug Log keeps periodic summaries; an additional `playback-trace.enabled` marker opts into per-event traces. New logs identify their UTF-8 encoding.
- Temporary lease expiry latches external audio off, stops old voices, restores native ownership and reloads samples before new playback lifetimes can use ASIO again. Queue/state loss remains fail-closed; failed restarts cannot silently regain external ownership.
- The backend wakes on queued events, and optional QPC diagnostics report enqueue-to-handler delay. Loop voice loss fails over only the affected playback lifetime; retired-generation history is bounded while stale PLAY events remain rejected.

Clean defaults are **16 samples / Driver ID 0 / 48000 Hz / Output Volume 50 / Debug Log 0 / Contract Mode 1 (Adaptive)**. Driver 0 is an initial value; select your own device before use. Language preferences are saved separately in `language.ini`. Changing driver, buffer or sample rate requires restarting the game.

## Compatibility and limits

Reviewed references are .NET 10 Game `2026.1026.0-tachyon` / `2026.1110.0-tachyon` and Framework `2026.1019.0` / `2026.1030.0`. Version 1.3.0 retains [three-tier contract validation](docs/AUDIO_CONTRACT.md). `LazerASIO.exe` always runs the Adaptive contract (`Contract Mode = 1`) and normalizes a hand-edited `0` back to Adaptive; Strict is not offered in the window. Adaptive requires structural signatures and critical semantic projections, permits unreviewed exact-hash drift only after these checks pass and displays `[Compat/Adaptive]`. `[Verified Contract]` identifies an exact match. Old five-field INIs are treated as Adaptive. A later client date/version alone does not establish compatibility.

This tool injects a native hook and installs runtime adapters in the game process. Understand the client's rules before use; this project makes no account-safety guarantee. It does not overwrite the game client, account configuration or beatmap database.

When the tool exits or fails, hitsounds fall back to the game's native output. If the game's Hitsound volume is 0%, that fallback is silent. A local channel adaptation error now rejects only that channel; contract/installation failures still disable external output globally.

Restart the game before reattaching after closing the tool. Multiple clients are handled one at a time. A loop already playing before attachment may require re-entering the beatmap. Software buffer duration is not a measured end-to-end latency.

## Source and development

Windows x64, MSVC C++17, Windows SDK and .NET 10 SDK are required to build. Run `Build.ps1` from an x64 Visual Studio developer shell (the window is native Win32 + Direct2D, no extra runtime), then export a clean portable package with `distribution/Build-Share.ps1`. See [build instructions](docs/BUILD.md), [CI results](https://github.com/comgrat/Lazer-Hitsound-ASIO/actions) and the [maintenance decisions and recovery plan](docs/MAINTENANCE.md). Report reproducible problems through [Issues](https://github.com/comgrat/Lazer-Hitsound-ASIO/issues).

User logs, device markers, language preferences, game files and development investigation reports are not included in this repository's new working tree or portable releases. The original upstream history is retained.
