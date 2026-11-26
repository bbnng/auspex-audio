#pragma once

#include <string>

namespace sa {

// Reflections of the room: hybrid = directional early reflections by convolution, then a parametric tail.
enum class ReverbMode { kParametric = 0, kHybrid = 1 };

struct EngineConfig {
    bool  enabled            = true;
    float master_volume      = 1.0f;
    bool  mute_in_background = true;  // silent while another window is in front, like the original DirectSound output
    bool  hrtf_enabled       = true;
    std::string  sofa_file;           // as the player wrote it; empty: the built-in HRTF
    std::wstring sofa_path;           // where it was found (resolve_sofa); empty although sofa_file is set: missing
    bool  occlusion_enabled  = true;
    int   occlusion_samples  = 16;
    bool  reverb_enabled     = true;
    float reverb_wet         = 0.55f;
    int   reflection_rays    = 1024;
    int   reflection_bounces = 4;
    float reverb_duration    = 1.25f;
    ReverbMode reverb_mode   = ReverbMode::kHybrid;
    float early_reflections  = 0.30f; // hybrid: seconds rendered by convolution
    bool  dynamic_doors      = true;
    bool  voice_directivity  = true;  // characters are a little softer and duller from behind (never in conversations)
    bool  weather_occlusion  = true;  // rain is muffled under a roof
    float weather_volume     = 0.65f; // rain that plays everywhere, relative to vanilla
    float rain_masking       = 1.0f;  // how much rain covers distant voices and the sounds of the city (0 = not at all)
    // Corrections of what the game itself gets wrong ([Tweaks]).
    bool  dialogue_glass     = true;  // a pane of glass between the player and who talks to them muffles the voice
    bool  fix_npc_barks      = true;  // passers-by that the game plays at full level in both ears
    bool  level_voices       = true;  // everybody speaks at one loudness
    bool  tame_loud_sounds   = true;  // frenzy and other hot 2D effects, bass that stacks up
    bool  fix_ambient_directions = true;  // street sounds come from the street, not out of the houses
    bool  filter_heels_ambients  = true;  // leaves out heels and steps that nobody makes
    bool  filter_hoof_ambient    = true;  // ... and the one that sounds like a horse
    int   sample_rate        = 0;     // 0: the device's own
};

} // namespace sa
