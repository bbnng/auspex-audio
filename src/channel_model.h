#pragma once

#include "coord_util.h"

#include <cstdint>

namespace sa {

// What engine.dll says about one playing channel_t this frame. Filled by engine_hooks, judged by describe_channel().
struct ChannelSnapshot {
    const char* name = "";      // CSfxTable name, may start with prefix characters ('*', '#', ')'...)
    int   entnum = 0;
    int   entchannel = 0;       // CHAN_*: 2 voice, 5 stream, 6 static
    float origin[3] = {};
    float dist_mult = 0.0f;     // 0: plays everywhere (SNDLVL_NONE)
    float radius = 0.0f;
    int   master_vol = 0;       // 0..255
    int   volumes[4] = {};      // left, right, rear left, rear right as S_SpatializeChannel left them
    float distance = 0.0f;      // to the listener, inches
    bool  dry = false;          // '#' or '>' file, or SND flag 0x800 (ambient_generic "flag_no_sfx", sound scheme "Dry")
    bool  skip_collide = false; // SND flag 0x200 or SNDLVL_NONE: never obscured by walls
    bool  no_duck = false;      // SND flag 0x1000
    bool  is_voice = false;
    bool  is_looped = false;
    bool  own = false;          // emitted by the view entity (the player)
    bool  player = false;       // emitted by the player's character, whatever the view is
    float duck = 1.0f;          // how loud everything but voices is right now
    float music_volume = 1.0f;  // snd_musicvolume (0..1, relative to master volume; e.g. UP12 Options Console)
    float effects_volume = 1.0f; // snd_effectsvolume (0..1, relative to master volume; e.g. UP12 Options Console)
    float mouth_height = 0.0f;  // a talking character: how far above its origin (the feet) the mouth is, inches
    bool  has_facing = false;   // a character whose angles are known
    float facing[3] = {};       // where it faces (Source axes)
    bool  facing_recorded = false; // '<' file: left = facing the listener, right = facing away. The engine blends them
    std::uintptr_t mixer = 0;
};

// Corrections of what the game itself gets wrong; each can be turned off ([Tweaks]).
struct ChannelTweaks {
    bool npc_barks = true;          // lines of passers-by that the game plays everywhere come from who says them
    bool level_voices = true;       // all voices at one loudness; street scenes that the game plays too loud
    bool tame_loud = true;          // hot 2D effects
    bool ambient_directions = true; // street sounds out of houses
    bool no_heels = true;           // leave out the steps in the street and in clubs that nobody makes
    bool no_hoofs = true;           // ... and the one that sounds like a horse
};

// Whether a recording's own loudness is evened out (VoiceLeveler; LoudnessLeveler for stingers).
enum class Leveling : unsigned char { kNone = 0, kBark = 1, kStinger = 2, kDialogue = 3, kCry = 4 };

struct ChannelDescriptor {
    bool  active       = false;
    bool  spatial_3d   = false;
    float origin[3]    = {};
    float dist_mult    = 0.0f;
    float radius       = 0.0f;
    float master_gain  = 1.0f; // ducked under dialogue; for 2D channels the volume the engine itself computed
    float loop_master  = 0.0f; // looping sounds: master_vol / 255, unducked (the engine culls loops by it)
    float reverb_gain  = 0.6f; // 0..1: share of the sound that excites the room (0: UI, music, dry files)
    float weather      = 0.0f; // 0..1: rain and thunder that play everywhere; muffled under a roof by this much
    float rain         = 0.0f; // 0..1: how hard the rain falls that this is the sound of
    bool  outdoor_ambient = false; // traffic, city, sea: what rain covers
    bool  free_ambient    = false; // one of those that the game puts at a random point around the player
    Leveling leveling     = Leveling::kNone;
    std::uint32_t voice_file = 0;    // voices: hashes of the file's name and of its folder (who speaks), for VoiceMemory
    std::uint32_t voice_speaker = 0;
    bool  is_voice     = false;
    bool  dialogue     = false; // a line of a conversation with the player: close, dry, never behind a wall
    bool  no_occlusion = false; // the engine never obscures it either
    bool  directional  = false; // a character's voice: softer and duller from behind
    float forward[3]   = {};    // where it faces (Source axes)
    int   entnum       = 0;
    int   entchannel   = 0;
    std::uintptr_t mixer = 0;  // the channel's CAudioMixer*: identifies the playing sound (0 = unknown)
};

constexpr int kChanVoice  = 2;
constexpr int kChanStream = 5;

// The engine makes the player's own weapons 2 dB louder than full scale; nothing else gets that far.
constexpr float kMaxGain2D = 1.26f;
// Volume of the loudest rain of the game's streets (sm_hub_1: 40 %).
constexpr float kFullRain = 0.40f;

// Music tracks under "music/" (after optional Source prefix chars and "sound/"), matching UP12 Options Console.
bool is_music_sound(const char* name);
// Outdoor rain and thunder by file name ("area/santa_monica/rain_light_loop.wav", "environmental/weather/rain1.wav").
// Whole words only (no "train", "drain", "brain"), never music, dialogue, interface sounds, or rain that drips in sewers.
bool is_weather_sound(const char* name);
// The part of it that is rain, not thunder.
bool is_rain_sound(const char* name);
// Traffic, sirens, the sea, steps in the distance, the city's background: what is heard outdoors and belongs there.
bool is_outdoor_ambient(const char* name);
// What characters say by themselves ("character/male/citizen_2/float_12.wav"). Not conversations ("character/dlg/..."),
// recordings on machines ("character/conversations/..."), monsters and animals.
bool is_bark_sound(const char* name);
// Shouts of scripted street characters (the prophet by the diner), played like stage lines.
bool is_street_scene_line(const char* name);
// Frenzy: ten seconds of drone in 2D, at full level over everything else. The other messages are short and stay.
bool is_stinger_sound(const char* name);
// Those that are no speech: pain, death, screams, laughter, feeding.
bool is_cry_sound(const char* name);
// Lines of conversations and story scenes ("character/dlg/...").
bool is_dialogue_sound(const char* name);
// Somebody's heels and steps that no one makes: sounds the maps' sound schemes play at a random place.
// "Area/Special/Tutorial/Pedestrianby1.wav", 2, 3 and 5; "Environmental/Interior/Ambient_High_Heels3.wav" and its likes.
// Never the steps of characters ("surfaces/...", "player/footsteps/...").
bool is_heels_ambient(const char* name);
// Its relative that sounds like a horse in an empty street: "Area/Special/Tutorial/Pedestrianby4.wav", an uneven trot.
bool is_hoof_ambient(const char* name);

// The share of a sound that engine.dll sends through its room DSP (SND_GetDspMix, RVA 0x119690, with the dsp_* defaults).
// Not read from the channel: it is 0 there while the player has the game's own room effects off (dsp_on 0).
float engine_room_share(float distance, float dist_mult, bool is_voice, const GainModel& model);

// The sound level of passers-by (SNDLVL of the game's own float lines), and the dist_mult of a level.
constexpr float kBarkLevelDb = 70.0f;
float dist_mult_for_level(float level_db, const GainModel& model);
float level_for_dist_mult(float dist_mult, const GainModel& model); // dist_mult > 0

ChannelDescriptor describe_channel(const ChannelSnapshot& c, const GainModel& model = GainModel(),
                                   const ChannelTweaks& tweaks = ChannelTweaks());

} // namespace sa
