Auspex Audio 1.0.0
==================

Modern 3D sound engine replacement for Vampire: The Masquerade - Bloodlines, powered by
Steam Audio (4.8.1) and miniaudio over Windows WASAPI.


REQUIREMENTS
------------
- Vampire: The Masquerade - Bloodlines with the Unofficial Patch (Basic or Plus)
- Bloodlines Script Extender (BLSE) 0.7 or newer


INSTALLATION
------------
1. Copy the Bin folder from this archive into your game folder (next to Vampire.exe),
   merging it with your existing Bin folder. The plugin installs to:
     Bin\loader\blse\mods\auspex_audio\
2. Launch the game through BLSE.
3. Check Bin\loader\blse\blse.log (or type sa_status in the console) to confirm Auspex Audio
   is active.


WHAT IT DOES
------------
- True 3D spatial audio (HRTF) over low-latency WASAPI (with stereo mode for speakers,
  custom .sofa HRTF support, and seamless audio device hot-swapping).
- Camera turns are spatialized directly in the audio callback without waiting on snd_mixahead.
- Real-time BSP map geometry occlusion, material-based transmission (concrete, wood, glass,
  metal, tile, carpet), and moving doors / brush entities.
- Raytraced room reflections and reverb (hybrid convolution + parametric tail, or parametric).
- Protective glass counter muffling in dialogues (pawn shop, blood bank) without dialogue echo.
- Rain and weather acoustics: roof muffling when stepping indoors, plus natural distance
  damping and outdoor ambient ducking while standing in the rain.
- Bidirectional voice loudness leveling (ITU-R BS.1770 K-weighted speech loudness with
  look-ahead), fixing overly loud and overly quiet voice lines across vanilla, Unofficial
  Patch, and voice mods.
- Optional tweaks: spatialization fix for vanilla 2D NPC barks (prostitutes, Chunk, etc.),
  NPC voice directivity, Frenzy / stacked combat bass guard, open-street ambient direction
  fixing, and phantom heels/hoof ambient filters.
- Full lip-sync and dialogue timing compatibility.


CONSOLE COMMANDS
----------------
- sa_preset headphones | speakers | subtle | cinematic
- sa_enable 0/1                   Toggle between Auspex Audio and vanilla DirectSound
- sa_hrtf 0/1                     Toggle HRTF binaural spatialization
- sa_sofa <file.sofa|default>     Load a custom .sofa HRTF from the mod folder or sofa\
- sa_occlusion 0/1                Toggle wall occlusion and material transmission
- sa_doors 0/1                    Toggle dynamic doors / moving brushes
- sa_reverb 0/1 [wet]             Toggle raytraced reverb and optional wet level (0.0-2.0)
- sa_reverb_mode hybrid|parametric
- sa_weather 0/1                  Toggle roof muffling of rain and thunder
- sa_weather_vol [0.0-2.0]        Outdoor rain loudness relative to vanilla
- sa_rain [0.0-2.0]               Rain distance damping and outdoor ambient masking
- sa_directivity 0/1              Toggle non-dialogue NPC voice facing directivity
- sa_tweaks [name] [0/1]          List or toggle [Tweaks] options
- sa_volume [0.0-2.0]             Master volume multiplier
- sa_mute_background 0/1          Fade to silence when Alt-Tabbed
- sa_save                         Save current settings to auspex_audio.ini
- sa_status / sa_debug            Inspect engine state, channels, reverb, and latency


SETTINGS
--------
Bin\loader\blse\mods\auspex_audio\auspex_audio.ini
Every option is explained inside the file. Settings changed in the console can be saved
back to the file with sa_save.


UNINSTALLING
------------
Delete Bin\loader\blse\mods\auspex_audio\


REPORTING A PROBLEM
-------------------
Include Bin\loader\blse\blse.log from the session where it happened, plus the output of
sa_status or sa_debug if relevant.


LICENSE
-------
Auspex Audio is free software under the GNU GPL version 3 (see LICENSE.txt and NOTICE.txt).
Built with Steam Audio (Apache 2.0, STEAM-AUDIO-LICENSE.txt), miniaudio (Unlicense / MIT-0,
MINIAUDIO-LICENSE.txt), and the BLSE plugin header (MIT, BLSE-LICENSE.txt).
Source code: https://github.com/bbnng/auspex-audio
