# Ionization

A visual changer for Windows 11, built around screen-wide, CapCut-style motion blur.

**[Download Ionization.exe](https://github.com/bobblyionized/Ionization/releases/latest/download/Ionization.exe)**: one file, no installer. It updates itself from this repo's Releases.

## What it does

- **Motion blur on everything**, in three modes:
  - Smooth: optical flow, so each pixel smears along its own motion, like CapCut.
  - Trail: an exponential frame trail.
  - Frame blend: averages the last N frames for a ghosting look.
  - A slider and a typed value set the strength.
  - **Quality** goes from Low to Ultra (8 to 64 samples along the motion).
  - A **frame rate limit** of 60, 120 (default), 144, 240 or Max sets how often the blur redraws. It can never exceed the display's own refresh rate.
- **Color**: exposure, contrast, saturation, vibrance, temperature and tint.
- **LUTs**: nine built-in looks, plus any `.cube` file you import.
- **Effects**: vignette, film grain, bloom, sharpen, chromatic aberration and night warmth.
- **Desktop clock** in three styles (Volt, Mono, Minimal). It can sit on top or on the desktop, and you can drag it anywhere.
- **Island**: a live pill at the top of the screen. It shows now playing (any app that uses Windows media controls) and mic and camera in-use dots. Hover it for a card with play/pause/skip, volume and brightness sliders, mic mute, a color picker, snip and a focus timer. It comes in Island or Notch shape, with Black, Graphite or Glass themes.
  - `Ctrl+Alt+M` mutes the mic, `Ctrl+Alt+N` opens the card and `Ctrl+Alt+P` picks a color.
  - Scripts can post a line with `Ionization.exe --island "Build finished"`.
- **Clipboard history**: everything you copy (text, links, images, files), kept on the island's Clipboard tab (`Ctrl+Alt+H`) and in the panel. Items can be pinned and copied again with one click.
  - It is saved encrypted with your Windows account.
  - Copies that password managers mark private are skipped.
- **Shelf**: drag files onto the island to park them, then drag them back out into any app.
- **Widgets**: weather (Open-Meteo, no account needed), a world clock, a calendar, a to-do list and live CPU/GPU/RAM/network.
  - They sit on the desktop or on top, and you can drag each one anywhere.
- **Live wallpaper** behind your desktop icons. Your Windows wallpaper setting is never changed.
  - Sources: five animated looks (Aurora, Flow, Grid Pulse, Ink, Mesh), an image with an optional slow drift, or a muted looping video decoded on the GPU.
  - It only draws while the desktop is visible. Games, maximized or fullscreen windows, the lock screen and battery saver pause it.
  - Animated looks render at half resolution.
- **Dock**: a floating, magnifying dock that replaces the Windows taskbar and gives it back when turned off.
- **Transparent taskbar** (one switch on the Dock page, or the tray menu): the taskbar hides until you hover the bottom edge, then pops up with just your icons. It has no background and no hover effects.
- **Memory** and **Macros**: background RAM trimming, plus a keyboard and mouse macro recorder and player.
- **Profiles**: Clean, Cinematic, Dreamy, Retro, Night, and your own saved looks.
- **Hide desktop icons** for a clean wallpaper (`Ctrl+Alt+D` or the tray menu). It uses Windows' own "Show desktop icons" switch, and the icons come back when you quit.
- **Hotkeys**: `Ctrl+Alt+B` toggles blur, `Ctrl+Alt+V` toggles color and effects, `Ctrl+Alt+C` toggles the clock, `Ctrl+Alt+D` hides or shows desktop icons. A tray icon is always there too.

## How it works

Windows has no global post-processing hook, so Ionization does what screen shaders do:

1. Each refresh, it captures the monitor on the GPU with **DXGI Desktop Duplication**.
2. It blends the frame with earlier ones in linear light (an RGBA16F accumulator), grades it, and applies the LUT and effects in HLSL.
3. It presents the result in a borderless, click-through, topmost **DirectComposition** layer.
4. That layer is marked `WDA_EXCLUDEFROMCAPTURE`, so it never captures itself and can't feed back into the image.

The panel is a WebView2 page embedded in the exe.

## Performance and Game Mode

Ionization **pauses itself when a game is focused**. That covers any fullscreen app, plus any exe in the game list, which is editable in **Performance**. While paused, the overlay and screen capture are fully torn down, so the game presents directly and runs at native speed. Games can opt in to keep effects, using the low-cost renderer.

Measured on an Intel Iris Xe laptop (i5-1145G7, 1920×1200 @ 60 Hz) with `tools/fpsbench`, a GPU-bound test game running at about 75–100 fps:

| Setup | Game FPS vs. no Ionization |
|---|---|
| Game Mode (paused) | 0% (overlay and capture are gone) |
| Effects kept, trail, v1.1 renderer | about −8% |
| Effects kept, trail, v1.0.6 | −12% |
| Effects kept, blend, v1.0.6 | −20% |

About 5 points of the kept cost come from Windows Desktop Duplication itself. Any whole-screen effect pays it. The rest is Ionization's rendering:
- one fused pass with 10-bit accumulators;
- an O(1) integer box sum for blend mode;
- half-resolution trails by default;
- no full-screen copy while the screen moves, and no drawing at all while it's still.

## Limits

- The mouse cursor stays sharp, because it draws above every window.
- Effects add about one frame of latency.
- Exclusive-fullscreen games bypass the overlay. Borderless fullscreen works.
- DRM-protected video captures as black under effects.
- Effects apply to one display at a time.

## Building

You need Visual Studio 2022 Build Tools (C++ and the Windows SDK) and CMake.

```
build.cmd
```

This produces `out\Ionization-dev.exe`. Local builds are stamped `0.0.0-dev` and never auto-update.

Every push to `main` runs `.github/workflows/release.yml`. That workflow builds `v1.0.<run>` and publishes it as the latest release, and installed copies pick it up within six hours (or right away with **Check now**).

UI work: set `IONIZATION_UI_DIR=<repo>\ui` and the exe serves `ui/` live from disk. You can also open `ui/index.html` through any static server, where a mock host stands in for the native side.
