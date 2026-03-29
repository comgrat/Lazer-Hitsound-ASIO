# osu! External ASIO Sound

Low-latency sample / hitsound forwarding for current `osu! lazer`, based on external ASIO playback through [FMOD Core](https://www.fmod.com) and a BASS/BASSmix hook.

## Attention

This tool injects into the game process and patches native audio APIs. Treat it as an offline / at-your-own-risk tool.

## What changed in this fork

- The old stable-era implementation relied on `BASS_SampleLoad(path)` and `BASS_ChannelPlay/Stop()`.
- Current `osu! lazer` loads samples from memory and routes playback through `BASSmix`.
- This fork hooks `BASS_SampleLoad(memory)`, `BASS_SampleGetChannel()`, `BASS_ChannelSetAttribute()`, `BASS_ChannelFlags()`, and `BASSmix` pause/unpause/remove events instead.
- Only sample-style audio is forwarded to ASIO. Music tracks still play through lazer's normal output path.
- Now support volume / debug log control in config file / program.

## Required libraries

For current Windows desktop lazer builds, use **x64** versions of all native dependencies:

- `fmod_vc.lib`
- `fmod.dll`
- `libMinHook.x64.lib`

They must all match the architecture of:

- `osu!asio_sound.exe`
- `osu!asio_sound.dll`
- the target `osu! lazer` process

## Compile

Use an x64 Visual Studio Developer Command Prompt.

1. Put x64 FMOD VC import library in `lib/` as `fmod_vc.lib`.
2. Put x64 `fmod.dll` next to the built executables or somewhere on `PATH`.
3. Put x64 MinHook library in `lib/` as `libMinHook.x64.lib`.
4. Run `Compile.bat` from an **x64** Visual Studio Developer Command Prompt.

The previous x86-only MinGW setup is no longer the target.

## How to use

### First time

You need:

- two audio output devices, for example onboard audio plus a USB DAC
- two headphones or speakers you can use at the same time

Install ASIO4ALL, plug the playback device in, and set your normal Windows default output separately for other programs.

Run `config.exe` or `osu!asio_sound.exe`, choose your ASIO device, and confirm playback works on the ASIO path.

If you hear crackling or nothing at all, increase ASIO buffer size.

You may still need to calibrate offset for your ASIO chain.

### Generally

1. Run `config.exe` if you need to change ASIO driver, sample rate, or buffer size.
2. Open `osu! lazer`.
3. Run `osu!asio_sound.exe`.
4. Play.
