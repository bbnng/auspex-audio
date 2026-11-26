# Auspex Audio

A [BLSE](https://gitlab.com/bigbannng/bloodlines-script-extender) sound engine replacement for *Vampire: The Masquerade - Bloodlines*, powered by **Steam Audio 4.8** and **miniaudio** over Windows WASAPI.

Vanilla Bloodlines mixes audio through Source 2004's software/DirectSound pipeline with basic left/right panning, no real geometry occlusion, and legacy hardware EAX reverb that broke after Windows Vista. Auspex Audio intercepts the game's dry channels before mixing and renders true binaural 3D spatialization, real-time BSP occlusion and reflections, rain acoustics, and voice loudness leveling without modifying game assets or breaking lip-sync and dialogue timing.

## Features

- **HRTF binaural 3D audio & low-latency WASAPI:** True front/back/height cues for headphones (plus stereo mode for speakers), custom `.sofa` HRTF support, device-native 44.1/48 kHz output, clean device hot-swap, and camera-turn spatialization decoupled from `snd_mixahead`.
- **Real-time BSP occlusion, materials & moving doors:** Extracts map collision geometry and brush models (`*N`) directly from `engine.dll` (with `.bsp` fallback) and raytraces occlusion and frequency-dependent material transmission (concrete, wood, glass, metal, tile, carpet).
- **Raytraced room reflections & reverb:** Hybrid directional early reflections + smooth tail (or parametric mode) driven by the actual room around the listener.
- **Dialogue glass muffling:** Dedicated glass-only ray probe muffles NPCs behind protective counters (such as the pawn shop and blood bank) without adding false occlusion or room echo to normal conversations.
- **Rain & weather acoustics:** Rain and thunder muffle smoothly under roofs and indoors; while standing in the rain, distant voices and outdoor city ambients subtly step back and lose treble while close speech stays clear.
- **Bidirectional voice loudness leveling (`level_voices`):** Signal-driven ITU-R BS.1770 K-weighted speech loudness leveler with 150 ms zero-latency look-ahead and per-speaker/file memory. Works automatically on vanilla, Unofficial Patch, and voice mods while keeping whispers soft and screams intense.
- **Vanilla audio fixes (`[Tweaks]`, all configurable):**
  - `fix_npc_barks`: Fixes the vanilla integer-truncation bug in `vampire.dll` where float lines of NPCs with dialogue (prostitutes, Chunk, etc.) played 2D map-wide.
  - `tame_loud_sounds`: Tames Frenzy and stacked combat bass with a look-ahead peak limiter ahead of the soft clipper.
  - `fix_ambient_directions`: Moves random street soundscape one-shots out of solid house walls onto open street directions.
  - `filter_heels_ambients` / `filter_hoof_ambient`: Optional filters for phantom background heel/hoof soundscape clips (never affects real player, NPC, or scripted Ocean House footsteps).
- **Compatibility:** Tested alongside the Unofficial Patch, *Engine Fixes and Tweaks*, and *Memory Limit Expansion* (including Process Split mode).

## Requirements

- *Vampire: The Masquerade - Bloodlines* with the Unofficial Patch (Basic or Plus)
- BLSE 0.7 or newer

## Installing

Copy the `Bin` folder from the release archive into your game folder (next to `Vampire.exe`). The mod installs to `Bin/loader/blse/mods/auspex_audio/`.

Settings are in `Bin/loader/blse/mods/auspex_audio/auspex_audio.ini` and can also be adjusted live in-game via console commands (`sa_preset`, `sa_reverb`, `sa_rain`, `sa_tweaks`, `sa_sofa`, `sa_status`, `sa_debug`, etc.) and saved with `sa_save`.

## Building

Requires Visual Studio with the C++ x86 tools.

```bat
build.cmd
```

This builds `build/auspex_audio.dll` and `build/offline_tests.exe`, then runs the full offline test suite. To also run the real-map geometry tests against a local Unofficial Patch install:

```bat
build\offline_tests.exe --maps "<path-to-game>\Unofficial_Patch\maps"
```

To build the release archives in `dist/`:

```bat
python tools/package_release.py
```

## License

GPL-3.0-only, with an attribution term; see [LICENSE](LICENSE) and [NOTICE](NOTICE).

Third-party components:
- **Steam Audio 4.8.1** (`third_party/steamaudio/`): Apache License 2.0 ([third_party/steamaudio/LICENSE](third_party/steamaudio/LICENSE))
- **miniaudio** (`third_party/miniaudio/`): Unlicense / MIT-0 ([third_party/miniaudio/LICENSE](third_party/miniaudio/LICENSE))
- **BLSE plugin header** (`include/blse_api.h`): MIT License ([include/BLSE-LICENSE.txt](include/BLSE-LICENSE.txt))
