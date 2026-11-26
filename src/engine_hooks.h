#pragma once

#include "blse_api.h"
#include "steam_audio_engine.h"

namespace sa {

// Installs byte-verified BLSE hooks on engine.dll's sound mixer and DirectSound transfer layer.
// Returns true if all required hooks were installed cleanly.
bool install_engine_sound_hooks(const BLSE_HostAPI* host, SteamAudioEngine* engine);

// After the game restarted its engine: some builds load engine.dll again, which then has no hooks. Safe if it did not.
bool reinstall_engine_sound_hooks(const BLSE_HostAPI* host);

// Removes all installed hooks (safe to call even if not installed).
void remove_engine_sound_hooks(const BLSE_HostAPI* host);

// Returns true if the engine sound hooks are currently active.
bool engine_sound_hooks_installed();

// Forgets every door and other brush entity (level change).
void reset_brush_tracking();
// Brush entities the client knows now, solid or not.
int tracked_brush_count();

// Called on the game's main thread after every rendered frame. nullptr: none.
void set_frame_callback(void (*callback)());

// sa_debug: one line or two per playing channel_t.
void dump_channels(void (*print)(const char* line));

// Clears the vanilla engine audio device buffer (DirectSound/WaveOut) so no stale audio
// loops when switching from vanilla DirectSound to Steam Audio + WASAPI.
void clear_vanilla_audio_buffer();

} // namespace sa
