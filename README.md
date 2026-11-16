# Lazer Hitsound ASIO

Standalone ASIO output for gameplay hitsounds on reviewed osu!lazer / Tachyon audio implementations. Music and UI effects keep the game's native output.

**[Download the Windows x64 portable release](https://github.com/comgrat/Lazer-Hitsound-ASIO/releases/latest)** · [五語使用說明 / Five-language guide](docs/USAGE.md) · [Build](docs/BUILD.md) · [Verification](docs/VERIFICATION.md)

本项目基于 [Mother-Ship/osu-lazer-External-ASIO-Sound](https://github.com/Mother-Ship/osu-lazer-External-ASIO-Sound) 修改；更早上游为 [XTXTMTXTX/osu-External-ASIO-Sound](https://github.com/XTXTMTXTX/osu-External-ASIO-Sound)。由 comgrat 独立维护，非上游官方发布。保留原作者署名和提交历史。

This fork is based on Mother-Ship's lazer adaptation and XTXTMTXTX's original project. It is independently maintained by **comgrat** and is not an official upstream release. See [AUTHORS](AUTHORS.md) and [third-party notices](THIRD_PARTY_NOTICES.md). **No project-wide license has been established**; [LICENSE](LICENSE) records the unresolved upstream and FMOD permissions.

## 使用 / Quick start

1. 从上面的 **Releases** 下载 ZIP 并完整解压；GitHub 的“Source code”压缩包是源码，不是可运行程序。
2. 打开 `config.exe`，选择语言及自己的 ASIO 驱动，保存配置。
3. 运行 `osu!asio_sound.exe`，保持等待窗口，再照常启动游戏。显示附加就绪后进入谱面。
4. 之后可用 `Change-Language.cmd` 切换语言；请重新打开已有工具窗口。

Download the portable ZIP, extract the entire folder, and open `config.exe` to choose your language and ASIO driver. Run `osu!asio_sound.exe`, keep its window open, and launch the game normally. Wait for attachment readiness before entering a beatmap. Use `Change-Language.cmd` to choose another language later.

Five languages are integrated into the same application: **简体中文 · 繁體中文 · English · Français · Español**.

## Behavior

- Routes identified gameplay hitsounds through the external FMOD ASIO backend, suppressing their corresponding native channels after successful takeover. BGM and UI effects remain native.
- The in-game Hitsound slider at **0% still allows ASIO hitsounds**. Set `Output Volume` in this tool's `config.ini` to control ASIO volume; **0 mutes ASIO**. Saving updates the volume in about 0.5 seconds.
- Preserves beatmap/sample gain, pan, playback rate and legitimate layered hitsounds. Layered sounds are not deduplicated by sample name or a time window.
- Detects normally launched clients without copying or starting the game. Exiting the game releases the ASIO device, and the tool keeps waiting for the next client.
- Supports renamed outer tool folders and paths containing spaces or Chinese text. Keep the internal file names, `managed` and `locales` folders intact.
- Diagnostic file/console writes run in the background so a slow log sink cannot serialize hit layers.

Clean defaults are **16 samples / Driver ID 0 / 48000 Hz / Output Volume 50 / Debug Log 0 / Contract Mode 1 (Adaptive)**. Driver 0 is an initial value; select your own device before use. Language preferences are saved separately in `language.ini`. Changing driver, buffer or sample rate requires restarting the game.

## Compatibility and limits

Reviewed references are .NET 10 Game `2026.1026.0-tachyon` / `2026.1110.0-tachyon` and Framework `2026.1019.0` / `2026.1030.0`. Version 1.1.0 uses [three-tier contract validation](docs/AUDIO_CONTRACT.md). Both modes require structural signatures and critical semantic projections. Default `Contract Mode = 1` (Adaptive) permits unreviewed exact-hash drift only after these checks pass and displays `[Compat/Adaptive]`. `Contract Mode = 0` (Strict) additionally requires an exact reviewed Game/Framework pair. `[Verified Contract]` identifies an exact match. Old five-field INIs default to Adaptive; restart the game after changing mode. A later client date/version alone does not establish compatibility.

This tool injects a native hook and installs runtime adapters in the game process. Understand the client's rules before use; this project makes no account-safety guarantee. It does not overwrite the game client, account configuration or beatmap database.

Restart the game before reattaching after closing the tool. Multiple clients are handled one at a time. A loop already playing before attachment may require re-entering the beatmap. Software buffer duration is not a measured end-to-end latency.

## Source and development

Windows x64, MSVC C++17, Windows SDK and .NET 10 SDK are required to build. Run `Build.ps1` from an x64 Visual Studio developer shell, then export a clean portable package with `distribution/Build-Share.ps1`. See [build instructions](docs/BUILD.md), [CI results](https://github.com/comgrat/Lazer-Hitsound-ASIO/actions) and the [maintenance decisions and recovery plan](docs/MAINTENANCE.md). Report reproducible problems through [Issues](https://github.com/comgrat/Lazer-Hitsound-ASIO/issues).

User logs, device markers, language preferences, game files and development investigation reports are not included in this repository's new working tree or portable releases. The original upstream history is retained.
