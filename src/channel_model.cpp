#include "channel_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace sa {
namespace {

bool starts_with_ci(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
        if ((c == '\\' ? '/' : c) != *prefix) return false;
    }
    return true;
}

// The path below "sound/" without the prefix characters that select how a file is played.
const char* bare_name(const char* name) {
    if (!name) return "";
    while (*name && std::strchr("*?!#@><^)/\\", *name)) ++name;
    return name;
}

bool starts_with_any(const char* name, const char* const* prefixes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (starts_with_ci(name, prefixes[i])) return true;
    }
    return false;
}

bool contains_ci(const char* s, const char* part) {
    for (; *s; ++s) {
        if (starts_with_ci(s, part)) return true;
    }
    return false;
}

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool is_letter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

// `word` starts at a word boundary and is either the whole word or followed by one of the given endings.
bool has_word(const char* s, const char* word, const char* const* endings, std::size_t ending_count) {
    const std::size_t len = std::strlen(word);
    for (const char* p = s; *p; ++p) {
        if ((p != s && is_letter(p[-1])) || !starts_with_ci(p, word)) continue;
        if (!is_letter(p[len])) return true;
        for (std::size_t i = 0; i < ending_count; ++i) {
            if (starts_with_ci(p + len, endings[i]) && !is_letter(p[len + std::strlen(endings[i])])) return true;
        }
    }
    return false;
}

// FNV-1a over the name as the file system sees it; the folder's first. 0 is never the result.
void hash_voice(const char* name, std::uint32_t& file, std::uint32_t& folder) {
    name = bare_name(name);
    std::uint32_t h = 2166136261u;
    folder = 0;
    for (; *name; ++name) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*name)));
        if (c == '\\') c = '/';
        if (c == '/') folder = h | 1u;
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    file = h | 1u;
}

} // namespace

bool is_rain_sound(const char* name) {
    name = bare_name(name);
    static const char* const kNever[] = { "music/", "character/", "interface/", "ui/", "radio/", "vo/" };
    if (starts_with_any(name, kNever, 6)) return false;
    static const char* const kRain[] = { "y", "fall", "storm", "drop", "drops", "ing" };
    return has_word(name, "rain", kRain, 6) || has_word(name, "drizzle", nullptr, 0);
}

bool is_weather_sound(const char* name) {
    name = bare_name(name);
    static const char* const kNever[] = { "music/", "character/", "interface/", "ui/", "radio/", "vo/" };
    if (starts_with_any(name, kNever, 6)) return false;
    static const char* const kThunder[] = { "storm", "clap", "ing", "s" };
    return is_rain_sound(name) || has_word(name, "thunder", kThunder, 4);
}

bool is_outdoor_ambient(const char* name) {
    name = bare_name(name);
    static const char* const kOutdoor[] = {
        "environmental/city/", "environmental/vehicles/", "environmental/outdoor/", "environmental/seafront/",
        "area/special/tutorial/pedestrianby", "area/santa_monica/santa monica main bg", "area/downtown/downtown_main",
    };
    if (!starts_with_any(name, kOutdoor, 7)) return false;
    static const char* const kIndoor[] = { "restaurant", "radio", "candle" };
    for (const char* word : kIndoor) {
        if (contains_ci(name, word)) return false;
    }
    return !is_weather_sound(name);
}

bool is_bark_sound(const char* name) {
    static const char* const kPeople[] = { "character/male/", "character/female/" };
    return starts_with_any(bare_name(name), kPeople, 2);
}

bool is_street_scene_line(const char* name) {
    return starts_with_ci(bare_name(name), "character/dlg/generic/");
}

bool is_stinger_sound(const char* name) {
    return starts_with_ci(bare_name(name), "interface/infobar/frenzy");
}

bool is_cry_sound(const char* name) {
    if (!is_bark_sound(name)) return false;
    const char* file = bare_name(name);
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') file = p + 1;
    }
    static const char* const kCries[] = { "pain", "death", "scream", "exert", "cry", "fear_", "laugh", "madness_loop", "maddness_loop",
                                          "panicrun", "vomit", "eyebleed", "feed_on", "fed_upon", "comatose", "theftofvitae",
                                          "screech", "wingflap", "fly_by", "land.", "fall." };
    return starts_with_any(file, kCries, sizeof(kCries) / sizeof(kCries[0]));
}

bool is_dialogue_sound(const char* name) {
    return starts_with_ci(bare_name(name), "character/dlg/");
}

bool is_hoof_ambient(const char* name) {
    return starts_with_ci(bare_name(name), "area/special/tutorial/pedestrianby4.");
}

bool is_heels_ambient(const char* name) {
    name = bare_name(name);
    static const char* const kAmbient[] = { "environmental/", "area/" };
    if (!starts_with_any(name, kAmbient, 2) || is_hoof_ambient(name)) return false;
    if (starts_with_ci(name, "area/special/tutorial/pedestrianby")) return true;
    static const char* const kMany[] = { "s" };
    return has_word(name, "heel", kMany, 1);
}

float dist_mult_for_level(float level_db, const GainModel& m) {
    return std::pow(10.0f, (m.ref_db - level_db) / 20.0f) / m.ref_dist;
}

float level_for_dist_mult(float dist_mult, const GainModel& m) {
    return m.ref_db - 20.0f * std::log10(m.ref_dist * dist_mult);
}

float engine_room_share(float distance, float dist_mult, bool is_voice, const GainModel& m) {
    constexpr float kMixMin = 0.6f, kMixMax = 0.99f, kDistMax = 1440.0f; // dsp_mix_min, dsp_mix_max, dsp_dist_max
    constexpr int   kDbMin = 70;                                         // dsp_db_min
    constexpr float kDbDrop = 0.85f, kVoiceScale = 0.85f, kVoiceMax = 0.5f; // dsp_db_mixdrop, dsp_voice_scalar/_max
    const float t = std::clamp(distance / kDistMax, 0.0f, 1.0f);
    float mix = kMixMin + (kMixMax - kMixMin) * t;
    const double denom = static_cast<double>(m.ref_dist) * static_cast<double>(dist_mult);
    const double level = denom > 0.0 ? std::log10(std::pow(10.0, m.ref_db / 20.0) / denom) * 20.0 : 0.0;
    if (static_cast<int>(level) <= kDbMin) mix *= kDbDrop;
    if (is_voice) mix = std::min(kVoiceMax, mix * kVoiceScale);
    return mix;
}

ChannelDescriptor describe_channel(const ChannelSnapshot& c, const GainModel& model, const ChannelTweaks& tweaks) {
    ChannelDescriptor d{};
    d.active = true;
    d.entnum = c.entnum;
    d.entchannel = c.entchannel;
    d.mixer = c.mixer;
    d.is_voice = c.is_voice;
    for (int i = 0; i < 3; ++i) d.origin[i] = c.origin[i];
    d.dist_mult = std::isfinite(c.dist_mult) ? std::max(0.0f, c.dist_mult) : 0.0f;
    d.radius = std::isfinite(c.radius) ? c.radius : 0.0f;

    const bool finite_pos = std::isfinite(c.origin[0]) && std::isfinite(c.origin[1]) && std::isfinite(c.origin[2]);

    // What people on the street say by themselves. CBaseCombatCharacter's float lines (vampire.dll 0x294F40) compute
    // their attenuation as SNDLVL_TO_ATTN(80) with integers if the character has a dialogue: 20 / 30 is 0, SNDLVL_NONE.
    // So the prostitutes, Chunk and others were heard in both ears, all over the map. The others' lines get 70 dB.
    const bool bark = c.entchannel == kChanVoice && !c.own && !c.player && is_bark_sound(c.name);
    const bool lost_bark = tweaks.npc_barks && bark && finite_pos && d.dist_mult <= 1e-9f;
    if (lost_bark) d.dist_mult = dist_mult_for_level(kBarkLevelDb, model);
    const bool skip_collide = c.skip_collide && !lost_bark; // SNDLVL_NONE brought it along
    const bool talks = c.is_voice || bark;

    const bool positional = finite_pos && d.dist_mult > 1e-9f;
    // S_SpatializeChannel plays the view entity's own sounds 12 units straight ahead of the camera, wherever the
    // player's body is (third person!): centred and at full level. Not a point in the world.
    d.spatial_3d = positional && !c.own;

    const float master = std::clamp(static_cast<float>(c.master_vol) / 255.0f, 0.0f, 1.0f);
    if (d.spatial_3d) {
        d.master_gain = master;
        if (c.is_looped) d.loop_master = master;
    } else {
        // No direction to replace, so the engine's own volume counts as it is: snd_gain, the distance gain of own
        // sounds and their +2 dB, the mixer's envelope and any fade. A centred sound gets half of it per side
        // (SpatializeChannel 0x117390), truncated to integers: the +1 takes that bias out.
        int sum = 0;
        for (int v : c.volumes) sum += std::clamp(v, 0, 255);
        d.master_gain = sum > 0 ? std::min(kMaxGain2D, static_cast<float>(sum + 1) / 255.0f) : 0.0f;
    }
    if (!c.is_voice && !c.no_duck) d.master_gain *= std::clamp(c.duck, 0.0f, 1.0f);

    // Conversations: CDialog plays the NPC's line as a stream with "skip collide" (0x102C0520 / 0x102C04B0 in
    // vampire.dll). The engine never obscures those lines. Ray tracing them muffled the voice whenever the dialogue
    // camera lost sight of the NPC's feet (bar, desk, chair), while the room answered at full level: an echo.
    d.no_occlusion = skip_collide;
    d.dialogue = c.is_voice && skip_collide && (c.entchannel == kChanVoice || c.entchannel == kChanStream);
    if (d.spatial_3d && talks && c.mouth_height > 0.0f) d.origin[2] += std::min(c.mouth_height, 100.0f);

    // All voices at one loudness, whatever they were recorded at. What passers-by and street characters say is also never
    // played louder than half way between 70 dB and the game's 80, which are at full level up to 10 m.
    const bool street_line = c.entchannel == kChanStream && c.is_voice && !c.own && !c.player && is_street_scene_line(c.name);
    if (tweaks.level_voices) {
        if (!d.dialogue && (bark || street_line)) {
            d.leveling = is_cry_sound(c.name) ? Leveling::kCry : Leveling::kBark;
            if (d.spatial_3d) {
                const float level = level_for_dist_mult(d.dist_mult, model);
                if (level > kBarkLevelDb) d.dist_mult = dist_mult_for_level(kBarkLevelDb + 0.5f * (level - kBarkLevelDb), model);
            }
        } else if (d.dialogue || is_dialogue_sound(c.name)) {
            d.leveling = Leveling::kDialogue;
        }
        if (d.leveling != Leveling::kNone) hash_voice(c.name, d.voice_file, d.voice_speaker);
    }
    if (tweaks.tame_loud && !d.spatial_3d && !c.is_voice && is_stinger_sound(c.name)) d.leveling = Leveling::kStinger;

    // What a character says by itself has a direction. A line spoken to the player does not: the dialogue camera cuts
    // and orbits around both, and the line has to stay equally clear.
    if (d.spatial_3d && talks && !d.dialogue && c.mouth_height > 0.0f && c.has_facing && !c.facing_recorded) {
        const float len = std::sqrt(c.facing[0] * c.facing[0] + c.facing[1] * c.facing[1] + c.facing[2] * c.facing[2]);
        if (std::isfinite(len) && len > 0.5f) {
            d.directional = true;
            for (int i = 0; i < 3; ++i) d.forward[i] = c.facing[i] / len;
        }
    }

    // engine.dll's room share: 0 for dry files, ~0.5-0.65 near, ~0.85-0.99 far away, voices at most 0.5.
    // Sounds that play everywhere (music, UI, weather, radio speech) do not stand in the room.
    d.reverb_gain = 0.0f;
    if (positional && !c.dry) {
        const float distance = (c.own || !std::isfinite(c.distance)) ? 0.0f : c.distance;
        d.reverb_gain = engine_room_share(distance, d.dist_mult, c.is_voice, model);
    }

    if (!positional && !c.is_voice && !c.no_duck && is_weather_sound(c.name)) {
        // Rain mixed in at 10 % or less is already heard from indoors (Ocean House), rain outside starts at 20 %.
        d.weather = smooth01((master - 0.10f) / 0.08f);
        if (is_rain_sound(c.name)) d.rain = d.weather * std::min(1.0f, master / kFullRain);
    }

    if (!c.is_voice && !c.own && is_outdoor_ambient(c.name)) {
        d.outdoor_ambient = true;
        // CSoundSchemeManager (vampire.dll 0x2298E0) puts the sounds of a "RandomSound" somewhere on a circle around the
        // player, through walls and all, and says that nothing obscures them.
        d.free_ambient = tweaks.ambient_directions && d.spatial_3d && skip_collide && !c.is_looped;
    }
    // Those of sound schemes only (nothing obscures them, see above): the Ocean House plays the same heels as a ghost's.
    const bool scheme_sound = c.skip_collide && !c.is_looped && !c.is_voice && !c.own && !c.player;
    if (scheme_sound && ((tweaks.no_heels && is_heels_ambient(c.name)) || (tweaks.no_hoofs && is_hoof_ambient(c.name)))) {
        d.master_gain = 0.0f;
        d.loop_master = 0.0f;
    }
    return d;
}

} // namespace sa
