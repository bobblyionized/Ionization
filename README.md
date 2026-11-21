# Ionization

A visual changer for Windows 11, built around screen-wide, CapCut-style motion blur.

**[Download Ionization.exe](https://github.com/bobblyionized/Ionization/releases/latest/download/Ionization.exe)**: one file, no installer. It updates itself from this repo's Releases.

## What it does

- **Motion blur on everything**, with two modes. Trail is an exponential frame trail; Frame blend averages the last N frames for a ghosting look. A slider and a typed value both set the strength, and a toggle turns it on and off.
- **Color**: exposure, contrast, saturation, vibrance, temperature and tint.
- **LUTs**: nine built-in looks, plus any `.cube` file you import.
- **Effects**: vignette, film grain, bloom, sharpen, chromatic aberration and night warmth.
- **Desktop clock** in three styles (Volt, Mono, Minimal). It can sit on top or on the desktop, and you can drag it anywhere.
- **Profiles**: Clean, Cinematic, Dreamy, Retro, Night, and your own saved looks.
- **Hotkeys**: `Ctrl+Alt+B` toggles blur, `Ctrl+Alt+V` toggles color and effects, `Ctrl+Alt+C` toggles the clock. A tray icon is always there too.

## How it works

Windows has no global post-processing hook, so Ionization does what screen shaders do:

1. Each refresh, it captures the monitor on the GPU with **DXGI Desktop Duplication**.
2. It blends the frame with earlier ones in linear light (an RGBA16F accumulator), grades it, and applies the LUT and effects in HLSL.
3. It presents the result in a borderless, click-through, topmost **DirectComposition** layer.
4. That layer is marked `WDA_EXCLUDEFROMCAPTURE`, so it never captures itself and can't feed back into the image.

The panel is a WebView2 page embedded in the exe.

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
