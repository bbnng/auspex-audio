#include "engine_hooks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sa {
namespace {

// Verified engine.dll RVAs (VTMB x86 build, image base 0x20000000).
constexpr std::uintptr_t kRvaChannels      = 0x1310B08; // channel_t[128], stride 0xA0
constexpr std::uintptr_t kRvaTotalChannels = 0x13107D4; // int
constexpr std::uintptr_t kRvaListenerOrg   = 0x13109CC; // float[3], written only by S_Update, every rendered frame
constexpr std::uintptr_t kRvaSoundTime     = 0x1307274; // int: DirectSound play position on the paintedtime clock
constexpr std::uintptr_t kRvaPaintedTime   = 0x1307278; // int (44100 Hz sample clock)
constexpr std::uintptr_t kRvaAudioDevice   = 0x136B9E0; // IAudioDevice*
constexpr std::uintptr_t kRvaVoiceScale    = 0x1B0FC0;  // float: ducking of non-voice sounds while a voice plays
constexpr std::uintptr_t kRvaVoiceChatScale = 0x1386E18; // int 0..256
constexpr std::uintptr_t kRvaViewEntity    = 0x315BE0;  // int, returned by IEngineSoundServices::GetViewEntity
constexpr std::uintptr_t kRvaGetViewEntity = 0x2590;    // mov eax, [view entity]; retn
constexpr std::uintptr_t kRvaSurroundFlag  = 0x130698C;
constexpr std::uintptr_t kRvaEntityList    = 0x1CC82C;  // IClientEntityList*
constexpr std::uintptr_t kRvaGetSpatial    = 0x2520;    // IEngineSoundServices::GetSoundSpatialization: uses that list
constexpr std::uintptr_t kRvaGetModelType  = 0xB5C10;   // CModelInfo::GetModelType: model_t::type
constexpr std::uintptr_t kRvaEngineClient  = 0x1974B0;  // IVEngineClient*
constexpr std::uintptr_t kRvaIsPaused      = 0x2620;    // calls its IsPaused (+0x3C); what MIX_MixChannels asks, too
constexpr std::uintptr_t kRvaGame          = 0x1301758; // CGame; +0x24: the window is active (WM_ACTIVATEAPP)
constexpr std::uintptr_t kRvaGamePtr       = 0x1AD5A8;  // IGame* game = that object
constexpr std::uintptr_t kRvaGameVtable    = 0x186CF8;
constexpr std::uintptr_t kRvaIsActiveApp   = 0xFEB70;   // CGame::IsActiveApp, vtable +0x28: mov al, [ecx+24h]; retn
constexpr std::size_t    kGameActive       = 0x24;
constexpr std::uintptr_t kRvaSoundFade     = 0x1307338; // float, percent: the `soundfade` command (S_GetMasterVolume)
constexpr std::uintptr_t kRvaSoundFadeRead = 0x118643;  // fld [that]

// Hook target RVAs:
constexpr std::uintptr_t kRvaSUpdate        = 0x120480;
constexpr std::uintptr_t kRvaSPaintChannels = 0x135AB0;
constexpr std::uintptr_t kRvaMixChannels    = 0x134530;
constexpr std::uintptr_t kRvaSFreeChannel   = 0x1344C0;
constexpr std::uintptr_t kRvaMix8Mono       = 0x116FE0;
constexpr std::uintptr_t kRvaMix8Stereo     = 0x117070;
constexpr std::uintptr_t kRvaMix16Mono      = 0x117100;
constexpr std::uintptr_t kRvaMix16Stereo    = 0x117190;
constexpr std::uintptr_t kRvaTransfer       = 0x117320;
constexpr std::uintptr_t kRvaClearBuffer    = 0x115B60;
constexpr std::uintptr_t kRvaSPause         = 0x13B850; // IAudioDevice::Pause if there is a device: while saving, system menu
constexpr std::uintptr_t kRvaSUnPause       = 0x13B860;

// channel_t (from StartSound 0x11EF50 / 0x11F620, S_SpatializeChannel 0x11E4B0, MIX_MixChannels 0x134530):
constexpr std::size_t kChSfx        = 0x00; // CSfxTable*; +0x104 CAudioSource*
constexpr std::size_t kChMixer      = 0x04; // CAudioMixer*
constexpr std::size_t kChVolumes    = 0x0C; // int[4]: left, right, rear left, rear right (0..255)
constexpr std::size_t kChEntNum     = 0x2C;
constexpr std::size_t kChEntChannel = 0x30;
constexpr std::size_t kChOrigin     = 0x34; // float[3], refreshed by S_SpatializeChannel every frame
constexpr std::size_t kChDistMult   = 0x50;
constexpr std::size_t kChMasterVol  = 0x54; // int 0..255
constexpr std::size_t kChFacing     = 0x68; // float -1..1
constexpr std::size_t kChDistMix    = 0x6C; // float 0..1
constexpr std::size_t kChDry        = 0x78; // byte: '#' file or SND flag 0x800: no room effect
constexpr std::size_t kChPrefix     = 0x7B; // char: '<' '>' '^' '@' ')' or 0
constexpr std::size_t kChRadius     = 0x7C;
constexpr std::size_t kChSkipCollide = 0x91; // byte: SND flag 0x200 or SNDLVL_NONE: never obscured
constexpr std::size_t kChNoDuck     = 0x9C; // StartSound flag 0x1000: not ducked by voices (this is not "is music")
constexpr std::size_t kSfxSource    = 0x104;
// CAudioSource vtable slots, called by engine.dll itself for every channel in S_Update / S_SpatializeChannel.
constexpr std::size_t kSourceIsVoice  = 0x10 / 4;
constexpr std::size_t kSourceIsLooped = 0x20 / 4;

// client.dll interfaces as engine.dll uses them (the same slots in all 270 entity classes of client.dll).
constexpr std::size_t kListGetEntity     = 0x0C / 4; // IClientEntityList
constexpr std::size_t kListHighestIndex  = 0x18 / 4;
constexpr std::size_t kEntGetCollideable = 0x08 / 4; // IClientEntity
constexpr std::size_t kEntGetNetworkable = 0x0C / 4;
constexpr std::size_t kEntGetRenderable  = 0x10 / 4;
constexpr std::size_t kEntGetAbsOrigin   = 0x24 / 4;
constexpr std::size_t kEntGetAbsAngles   = 0x28 / 4;
constexpr std::size_t kNetIsDormant      = 0x20 / 4; // IClientNetworkable
constexpr std::size_t kRenGetModel       = 0x20 / 4; // IClientRenderable
constexpr std::size_t kColMaxs           = 0x08 / 4; // ICollideable
constexpr std::size_t kColGetSolid       = 44 / 4;
constexpr std::size_t kColGetSolidFlags  = 48 / 4;
constexpr std::size_t kModelType         = 0x88;     // model_t: name[] at 0 ("*N" for brush models), type 1 = brush
constexpr int kModBrush         = 1;
constexpr int kFsolidNotSolid   = 4;
constexpr int kFsolidTrigger    = 8;
constexpr int kMaxClientEntities = 8192;
constexpr int kPlayerEntity = 1; // single player
constexpr int kScanPerFrame     = 128;

constexpr int kPaintBuffer = 1020;

constexpr std::size_t kChannelStride = 0xA0;

// Source 2003 fixed-point shift for inputOffset & rateScaleFix (verified in FUN_20136ea0 / FUN_20137110).
constexpr uint32_t kFixShift = 28;
constexpr uint32_t kFixMask  = (1u << kFixShift) - 1u;

using SUpdateFn        = int (__cdecl*)(const float* origin, const float* forward, const float* right, const float* up);
using ClearBufferFn    = void (__fastcall*)(void* thisptr, void* edx);
using PauseFn          = void (__cdecl*)();
using SPaintChannelsFn = void (__cdecl*)(int endtime);
using MixChannelsFn    = void (__cdecl*)(int endtime, int mixgroup, int sampleRate, int sampleRate2);
using SFreeChannelFn   = void (__cdecl*)(void* ch);
using MixAudioFn       = void (__fastcall*)(void* thisptr, void* edx,
                                            void* pChannel, void* pData,
                                            int outputOffset, int inputOffset,
                                            uint32_t rateScaleFix, int outCount, int timecompress);
using TransferFn       = void (__fastcall*)(void* thisptr, void* edx, int endtime);

const BLSE_HostAPI* g_host = nullptr;
SteamAudioEngine* g_engine = nullptr;
unsigned char* g_engine_base = nullptr;
bool g_hooks_installed = false;

SUpdateFn        orig_s_update         = nullptr;
ClearBufferFn    orig_clear_buffer     = nullptr;
SPaintChannelsFn orig_s_paint_channels = nullptr;
MixChannelsFn    orig_mix_channels     = nullptr;
SFreeChannelFn   orig_s_free_channel   = nullptr;
MixAudioFn       orig_mix8_mono        = nullptr;
MixAudioFn       orig_mix8_stereo      = nullptr;
MixAudioFn       orig_mix16_mono       = nullptr;
MixAudioFn       orig_mix16_stereo     = nullptr;
TransferFn       orig_transfer         = nullptr;
PauseFn          orig_s_pause          = nullptr;
PauseFn          orig_s_unpause        = nullptr;

// Per-paint-chunk state (all sound mixing runs on the game's main thread).
int  g_current_mix_rate = kDryRate;
bool g_in_paint_chunk = false;
bool g_view_entity_known = false;
bool g_entities_known = false;
bool g_pause_known = false;
bool g_focus_known = false;
bool g_fade_known = false;
bool g_foreign_window = false;
bool g_clearing_own_buffer = false;
uint32_t g_paint_chunk_seq = 0;

// Deduplicate multi-paintbuffer passes: Source mixes each channel into 1..3 paintbuffers
// (IPAINTBUFFER, IROOMBUFFER, IFACINGBUFFER) within a single MixDataToDevice call.
// We capture only the first pass for each (channel_idx, chunk_seq, outputOffset, outCount).
struct CaptureDedup {
    uint32_t chunk_seq = 0;
    int      last_rate = 0;
    int      last_out_offset = -1;
    int      last_out_count = -1;
};
CaptureDedup g_dedup[kMaxChannels];

// Last input sample per channel: upsampling interpolates across Mix* calls and paint chunks without steps.
struct UpsampleHistory {
    std::uintptr_t mixer = 0;
    float l = 0.0f;
    float r = 0.0f;
};
UpsampleHistory g_upsample[kMaxChannels];

// Reusable scratch buffers for resampling a single Mix* call into 44100 Hz float.
float g_tmp_l[2048];
float g_tmp_r[2048];
float g_up_l[2048];
float g_up_r[2048];

GainModel g_gain_model;
ChannelTweaks g_tweaks;
float g_volume = 1.0f;
float g_music_volume = 1.0f;
float g_effects_volume = 1.0f;
uint32_t g_frame_counter = 0;

// Watches the DirectSound clock: without it the game plays no sound at all, here or otherwise.
struct {
    int sound_time = 0;
    uint32_t frames = 0;
    ULONGLONG since_ms = 0;
    bool reported = false;
} g_clock;
void (*g_frame_callback)() = nullptr;

void logf(const char* fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(buf);
}

int channel_index_from_ptr(const void* ch_ptr) {
    if (!g_engine_base || !ch_ptr) return -1;
    const auto base = reinterpret_cast<std::uintptr_t>(g_engine_base + kRvaChannels);
    const auto p    = reinterpret_cast<std::uintptr_t>(ch_ptr);
    if (p < base) return -1;
    const std::uintptr_t diff = p - base;
    if ((diff % kChannelStride) != 0) return -1;
    const int idx = static_cast<int>(diff / kChannelStride);
    return (idx >= 0 && idx < kMaxChannels) ? idx : -1;
}

template <class T>
T read_at(const void* base, std::size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return v;
}

bool source_flag(const void* sfx, std::size_t slot) {
    void* source = sfx ? read_at<void*>(sfx, kSfxSource) : nullptr;
    if (!source) return false;
    using BoolFn = bool (__thiscall*)(void*);
    return (*reinterpret_cast<BoolFn* const*>(source))[slot](source);
}

// How loud engine.dll plays everything but voices right now (ducked under dialogue), as in sub_20135F80.
float duck_scale() {
    float scale = read_at<float>(g_engine_base, kRvaVoiceScale);
    if (!std::isfinite(scale)) scale = 1.0f;
    scale = std::clamp(scale, 0.0f, 1.0f);
    const int chat = read_at<int>(g_engine_base, kRvaVoiceChatScale);
    if (chat > 0 && chat < 256) scale = std::min(scale, static_cast<float>(chat) / 256.0f);
    return scale;
}

template <class R, class... Args>
R vcall(void* object, std::size_t slot, Args... args) {
    using Fn = R (__thiscall*)(void*, Args...);
    return (*reinterpret_cast<Fn* const*>(object))[slot](object, args...);
}

struct RawEntity { // plain data: filled under SEH
    bool  found;       // the client knows the entity
    int   model_index; // N of "*N", 0: no brush model
    bool  dormant;     // out of the player's sight; the state is the last known one
    bool  solid;
    float origin[3];
    float angles[3];
    float maxs_z;
};

void* client_entity(int index) {
    void* list = *reinterpret_cast<void* const*>(g_engine_base + kRvaEntityList);
    return (list && index > 0) ? vcall<void*>(list, kListGetEntity, index) : nullptr;
}

int highest_client_entity() {
    __try {
        void* list = *reinterpret_cast<void* const*>(g_engine_base + kRvaEntityList);
        return list ? std::clamp(vcall<int>(list, kListHighestIndex), 0, kMaxClientEntities - 1) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Brush entities only: for everything else just found and model_index = 0 are set.
bool read_brush_entity(int index, RawEntity* out) {
    std::memset(out, 0, sizeof(*out));
    __try {
        void* ent = client_entity(index);
        if (!ent) return true;
        out->found = true;
        void* renderable = vcall<void*>(ent, kEntGetRenderable);
        const char* model = renderable ? static_cast<const char*>(vcall<void*>(renderable, kRenGetModel)) : nullptr;
        if (!model || model[0] != '*' || *reinterpret_cast<const int*>(model + kModelType) != kModBrush) return true;
        int n = 0;
        for (int i = 1; i < 6 && model[i] >= '0' && model[i] <= '9'; ++i) n = n * 10 + (model[i] - '0');
        void* net = vcall<void*>(ent, kEntGetNetworkable);
        void* col = vcall<void*>(ent, kEntGetCollideable);
        const float* origin = vcall<const float*>(ent, kEntGetAbsOrigin);
        const float* angles = vcall<const float*>(ent, kEntGetAbsAngles);
        if (!net || !col || !origin || !angles) return true;
        out->dormant = vcall<bool>(net, kNetIsDormant);
        const int flags = vcall<int>(col, kColGetSolidFlags) & 0xFFFF;
        out->solid = (vcall<int>(col, kColGetSolid) & 0xFF) != 0 && (flags & (kFsolidNotSolid | kFsolidTrigger)) == 0;
        for (int k = 0; k < 3; ++k) {
            out->origin[k] = origin[k];
            out->angles[k] = angles[k];
        }
        out->model_index = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, sizeof(*out));
        return false;
    }
}

// How far above an entity's origin (its feet) a standing character's mouth is, and where it faces; 0 if this is no such
// entity or the sound does not come from its origin.
float read_talker(int index, const float sound_origin[3], float* yaw, bool* yaw_known) {
    *yaw_known = false;
    if (!g_entities_known || index <= 0 || index >= kMaxClientEntities) return 0.0f;
    __try {
        void* ent = client_entity(index);
        if (!ent) return 0.0f;
        const float* origin = vcall<const float*>(ent, kEntGetAbsOrigin);
        void* col = vcall<void*>(ent, kEntGetCollideable);
        const float* maxs = col ? vcall<const float*>(col, kColMaxs) : nullptr;
        if (!origin || !maxs) return 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (!(std::fabs(origin[k] - sound_origin[k]) < 12.0f)) return 0.0f;
        }
        if (!(maxs[2] >= 40.0f && maxs[2] <= 120.0f)) return 0.0f;
        const float* angles = vcall<const float*>(ent, kEntGetAbsAngles);
        if (angles && std::fabs(angles[1]) < 1.0e4f) {
            *yaw = angles[1];
            *yaw_known = true;
        }
        return 0.88f * maxs[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *yaw_known = false;
        return 0.0f;
    }
}

// Where the player's head is, also while the camera is somewhere else.
bool read_player_head(float out[3]) {
    if (!g_entities_known) return false;
    __try {
        void* ent = client_entity(kPlayerEntity);
        if (!ent) return false;
        const float* origin = vcall<const float*>(ent, kEntGetAbsOrigin);
        void* col = vcall<void*>(ent, kEntGetCollideable);
        const float* maxs = col ? vcall<const float*>(col, kColMaxs) : nullptr;
        if (!origin || !maxs || !(maxs[2] > 0.0f && maxs[2] <= 120.0f)) return false;
        for (int k = 0; k < 3; ++k) out[k] = origin[k];
        out[2] += 0.88f * maxs[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Doors, elevators, breakables, func_brushes: the client's view of them, once per rendered frame.
std::vector<BrushEntityState> g_brushes;
bool g_brush_tracked[kMaxClientEntities] = {};
int  g_brush_scan_next = 1;
int  g_brush_faults = 0;

void poll_brush_entities() {
    if (!g_entities_known) return;
    bool changed = false;
    RawEntity raw;
    for (std::size_t i = 0; i < g_brushes.size();) {
        BrushEntityState& b = g_brushes[i];
        const bool ok = read_brush_entity(b.entity, &raw);
        if (!ok) ++g_brush_faults;
        if (!ok || !raw.found || raw.model_index != b.model_index) {
            g_brush_tracked[b.entity] = false;
            b = g_brushes.back();
            g_brushes.pop_back();
            changed = true;
            continue;
        }
        if (!raw.dormant && (raw.solid != b.solid || std::memcmp(raw.origin, b.origin, sizeof(b.origin)) != 0 ||
                             std::memcmp(raw.angles, b.angles, sizeof(b.angles)) != 0)) {
            b.solid = raw.solid;
            std::memcpy(b.origin, raw.origin, sizeof(b.origin));
            std::memcpy(b.angles, raw.angles, sizeof(b.angles));
            changed = true;
        }
        ++i;
    }
    // New ones appear as the player comes near them.
    const int highest = highest_client_entity();
    for (int n = std::min(kScanPerFrame, highest); n > 0; --n) {
        if (g_brush_scan_next > highest) g_brush_scan_next = 1;
        const int index = g_brush_scan_next++;
        if (g_brush_tracked[index]) continue;
        if (!read_brush_entity(index, &raw)) {
            ++g_brush_faults;
            continue;
        }
        if (!raw.found || raw.model_index <= 0) continue;
        BrushEntityState b{};
        b.entity = index;
        b.model_index = raw.model_index;
        b.solid = raw.solid;
        std::memcpy(b.origin, raw.origin, sizeof(b.origin));
        std::memcpy(b.angles, raw.angles, sizeof(b.angles));
        g_brushes.push_back(b);
        g_brush_tracked[index] = true;
        changed = true;
    }
    if (g_brush_faults > 16) {
        g_entities_known = false; // not the client.dll this was written for
        g_brushes.clear();
        changed = true;
        logf("auspex_audio: reading client entities failed; doors are not tracked from here on.");
    }
    if (changed) g_engine->update_brush_entities(g_brushes.data(), g_brushes.size());
}

void read_channel_snapshot(const void* ch, ChannelSnapshot& c) {
    const void* sfx = read_at<const void*>(ch, kChSfx);
    c.name = sfx ? static_cast<const char*>(sfx) : "";
    c.entnum = read_at<int>(ch, kChEntNum);
    c.entchannel = read_at<int>(ch, kChEntChannel);
    c.mixer = read_at<std::uintptr_t>(ch, kChMixer);
    for (int i = 0; i < 3; ++i) c.origin[i] = read_at<float>(ch, kChOrigin + 4u * i);
    c.dist_mult = read_at<float>(ch, kChDistMult);
    c.radius = read_at<float>(ch, kChRadius);
    c.master_vol = read_at<int>(ch, kChMasterVol);
    for (int i = 0; i < 4; ++i) c.volumes[i] = read_at<int>(ch, kChVolumes + 4u * i);
    const auto* listener = reinterpret_cast<const float*>(g_engine_base + kRvaListenerOrg);
    const float dx = c.origin[0] - listener[0], dy = c.origin[1] - listener[1], dz = c.origin[2] - listener[2];
    c.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    c.dry = read_at<uint8_t>(ch, kChDry) != 0 || read_at<char>(ch, kChPrefix) == '>';
    c.skip_collide = read_at<uint8_t>(ch, kChSkipCollide) != 0;
    c.no_duck = read_at<uint8_t>(ch, kChNoDuck) != 0;
    c.is_voice = source_flag(sfx, kSourceIsVoice);
    c.is_looped = source_flag(sfx, kSourceIsLooped);
    c.own = g_view_entity_known && c.entnum == read_at<int>(g_engine_base, kRvaViewEntity);
    c.player = c.entnum == kPlayerEntity;
    c.duck = duck_scale();
    c.music_volume = g_music_volume;
    c.effects_volume = g_effects_volume;
    if (!c.own && (c.is_voice || (c.entchannel == kChanVoice && is_bark_sound(c.name)))) {
        float angles[3] = {}; // the body stands upright, whatever its entity's pitch and roll say
        c.mouth_height = read_talker(c.entnum, c.origin, &angles[1], &c.has_facing);
        if (c.has_facing) source_angles_forward(angles, c.facing);
        c.facing_recorded = read_at<char>(ch, kChPrefix) == '<';
    }
}

ChannelDescriptor read_channel_descriptor(const void* ch) {
    ChannelSnapshot c{};
    read_channel_snapshot(ch, c);
    return describe_channel(c, g_gain_model, g_tweaks);
}

float read_cvar(const char* name, float fallback) {
    if (!g_host || !BLSE_HOST_HAS(g_host, cvar_get)) return fallback;
    char buf[32];
    size_t n = 0;
    if (!g_host->cvar_get(g_host->context, name, buf, sizeof(buf), &n) || n == 0) return fallback;
    const float v = static_cast<float>(std::atof(buf));
    return std::isfinite(v) ? v : fallback;
}

void poll_cvars() {
    g_volume = read_cvar("volume", 1.0f);
    // Unofficial Patch 12 Options Console (options-console.vtm) adds Music/Sound sliders via these cvars and applies
    // them inside MIX_ScaleChannelVolume (0x135F80), which Auspex Audio's Mix* hooks bypass.
    g_music_volume = std::clamp(read_cvar("snd_musicvolume", 1.0f), 0.0f, 1.0f);
    g_effects_volume = std::clamp(read_cvar("snd_effectsvolume", 1.0f), 0.0f, 1.0f);
    if ((g_frame_counter & 63u) != 0u && g_frame_counter != 0u) return;
    const GainModel def{};
    g_gain_model.snd_gain   = std::max(0.0f, read_cvar("snd_gain", def.snd_gain));
    g_gain_model.gain_max   = std::max(0.0f, read_cvar("snd_gain_max", def.gain_max));
    g_gain_model.gain_min   = std::max(0.0f, read_cvar("snd_gain_min", def.gain_min));
    g_gain_model.foliage_db = std::max(0.0f, read_cvar("snd_foliage_db_loss", def.foliage_db));
    g_gain_model.ref_db     = read_cvar("snd_refdb", def.ref_db);
    g_gain_model.ref_dist   = std::max(1.0f, read_cvar("snd_refdist", def.ref_dist));
    g_engine->set_gain_model(g_gain_model);
}

void poll_player() {
    float head[3] = {};
    g_engine->update_player_head(read_player_head(head) ? head : nullptr);
}

bool game_paused() {
    if (!g_pause_known) return false;
    __try {
        return reinterpret_cast<bool (__cdecl*)()>(g_engine_base + kRvaIsPaused)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pause_known = false;
        return false;
    }
}

// The original's DirectSound buffer has no GLOBALFOCUS: Windows mutes it while another program's window is in front.
// engine.dll itself only notes that in a flag. A window of this process (BLSE's, a debug console) is no other program.
bool game_focused() {
    if (!g_focus_known) return true;
    if (read_at<unsigned char>(g_engine_base, kRvaGame + kGameActive) != 0) {
        g_foreign_window = false;
        return true;
    }
    const HWND front = GetForegroundWindow(); // none for a moment while Windows switches: nothing changes
    DWORD pid = 0;
    if (front && GetWindowThreadProcessId(front, &pid) != 0) g_foreign_window = pid != GetCurrentProcessId();
    return !g_foreign_window;
}

void poll_game_state() {
    g_engine->set_focused(game_focused());
    g_engine->set_game_paused(game_paused());
    // As in S_GetMasterVolume (0x118640).
    const float fade = g_fade_known ? read_at<float>(g_engine_base, kRvaSoundFade) : 0.0f;
    const float left = std::isfinite(fade) ? 1.0f - std::clamp(fade * 0.01f, 0.0f, 1.0f) : 1.0f;
    g_engine->set_engine_volume(g_volume * left);
}

void watch_sound_clock() {
    const int now = read_at<int>(g_engine_base, kRvaSoundTime);
    const ULONGLONG ms = GetTickCount64();
    if (now != g_clock.sound_time || g_clock.since_ms == 0) {
        if (g_clock.reported) logf("auspex_audio: the game's DirectSound clock runs again.");
        g_clock.sound_time = now;
        g_clock.since_ms = ms;
        g_clock.frames = 0;
        g_clock.reported = false;
        return;
    }
    if (++g_clock.frames < 60 || ms - g_clock.since_ms < 1500 || g_clock.reported) return;
    g_clock.reported = true;
    logf("auspex_audio: the game's DirectSound clock stands still, so the game mixes no new sound (no DirectSound device, or lost with it).");
}

void drain_messages() {
    std::string text;
    while (g_engine->pop_message(text)) {
        logf("auspex_audio: %s", text.c_str());
        if (BLSE_HOST_HAS(g_host, console_print)) {
            g_host->console_print(g_host->context, ("auspex_audio: " + text + "\n").c_str());
        }
    }
}

// Printable copy of a sound's name, whatever the pointer turns out to be.
void copy_name(const void* sfx, char* out, std::size_t cap) {
    out[0] = '\0';
    if (!sfx || cap == 0) return;
    __try {
        const char* p = static_cast<const char*>(sfx);
        std::size_t n = 0;
        for (; n + 1 < cap && p[n] != '\0'; ++n) out[n] = (p[n] >= 32 && p[n] < 127) ? p[n] : '?';
        out[n] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// Plain reads of one channel_t for sa_debug, whatever state it is in.
bool copy_channel(const void* ch, unsigned char* out) {
    __try {
        std::memcpy(out, ch, kChannelStride);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Publishes every playing channel's position and volume, and retires voices whose channel is gone. engine.dll
// also frees channels inline in MIX_MixChannels (a sound that ended), without calling S_FreeChannel.
void sync_channels(bool publish) {
    if (!g_engine || !g_engine_base) return;
    const unsigned char* ch = g_engine_base + kRvaChannels;
    for (int i = 0; i < kMaxChannels; ++i, ch += kChannelStride) {
        if (!g_engine->channel_has_voice(i)) continue;
        if (read_at<const void*>(ch, kChSfx) == nullptr) {
            g_engine->notify_channel_freed(i);
        } else if (publish) {
            g_engine->update_channel(i, read_channel_descriptor(ch));
        }
    }
}

// MIX_MixChannels skips a looping channel while all four of its volumes are below 6. Those volumes also contain
// the engine's coarse line-of-sight loss and the pan, so a hum would switch on and off with a camera turn or a wall
// in between. Steam Audio replaces both, and fades a loop to zero by distance alone (see kCullLevel), so keep a loop
// mixing for as long as it can be heard. Mix* ignores these volumes while the plugin is enabled.
void keep_audible_loops_mixing() {
    const auto* listener = reinterpret_cast<const float*>(g_engine_base + kRvaListenerOrg);
    const int total = std::clamp(read_at<int>(g_engine_base, kRvaTotalChannels), 0, kMaxChannels);
    unsigned char* ch = g_engine_base + kRvaChannels;
    for (int i = 0; i < total; ++i, ch += kChannelStride) {
        const void* sfx = read_at<const void*>(ch, kChSfx);
        if (!sfx) continue;
        auto* vol = reinterpret_cast<int*>(ch + kChVolumes);
        if (vol[0] > 5 || vol[1] > 5 || vol[2] > 5 || vol[3] > 5) continue;
        const ChannelDescriptor d = read_channel_descriptor(ch);
        if (d.loop_master <= 0.0f) continue;
        const float dx = d.origin[0] - listener[0], dy = d.origin[1] - listener[1], dz = d.origin[2] - listener[2];
        const float dist_m = std::sqrt(dx * dx + dy * dy + dz * dz) * kSourceToMeters;
        if (d.loop_master * compute_source_distance_attenuation(dist_m, d.dist_mult, g_gain_model) >= kCullLevel) {
            vol[0] = 6;
        }
    }
}

void ensure_chunk_started() {
    if (g_in_paint_chunk || !g_engine || !g_engine_base) return;
    g_in_paint_chunk = true;
    ++g_paint_chunk_seq;
    // Start with the paintbuffer's capacity; the actual length is finalized in TransferSamples.
    g_engine->begin_chunk(kPaintBuffer);
}

// Upsamples `in_count` samples at `rate` (11025, 22050 or 44100) to 44100 Hz. Interpolates from the previous call's
// last sample, so there is no step where two calls meet (a quarter-rate sound would otherwise buzz at 43 Hz).
int upsample_to_44100(const float* in, int in_count, int rate, float& history, float* out, int out_cap) {
    if (in_count <= 0 || out_cap <= 0) return 0;
    const int factor = (rate == 11025) ? 4 : (rate == 22050) ? 2 : 1;
    int n = 0;
    float prev = history;
    for (int i = 0; i < in_count && n + factor <= out_cap; ++i) {
        const float step = (in[i] - prev) / static_cast<float>(factor);
        for (int k = 1; k < factor; ++k) out[n++] = prev + step * static_cast<float>(k);
        out[n++] = in[i];
        prev = in[i];
    }
    history = prev;
    return n;
}

UpsampleHistory& upsample_history(int ch_idx, const void* ch) {
    UpsampleHistory& h = g_upsample[ch_idx];
    const std::uintptr_t mixer = read_at<std::uintptr_t>(ch, kChMixer);
    if (h.mixer != mixer) h = UpsampleHistory{ mixer, 0.0f, 0.0f };
    return h;
}

// Weights that fold a positional stereo file to its single source, per file-name prefix (sub_20136EA0 / sub_20137110):
// '<' left = facing the listener, right = facing away; '^' left = near, right = far recording.
void stereo_fold_weights(const void* ch, float& wl, float& wr) {
    wl = wr = 0.5f;
    const char prefix = read_at<char>(ch, kChPrefix);
    if (prefix == '<') {
        wl = std::clamp((read_at<float>(ch, kChFacing) + 1.0f) * 0.5f, 0.0f, 1.0f);
        wr = 1.0f - wl;
    } else if (prefix == '^') {
        wr = std::clamp(read_at<float>(ch, kChDistMix), 0.0f, 1.0f);
        wl = 1.0f - wr;
    }
}

bool should_capture(int ch_idx, int outputOffset, int outCount) {
    if (ch_idx < 0 || ch_idx >= kMaxChannels) return false;
    auto& d = g_dedup[ch_idx];
    if (d.chunk_seq == g_paint_chunk_seq &&
        d.last_rate == g_current_mix_rate &&
        d.last_out_offset == outputOffset &&
        d.last_out_count == outCount) {
        return false; // duplicate paintbuffer pass for same channel chunk
    }
    d.chunk_seq = g_paint_chunk_seq;
    d.last_rate = g_current_mix_rate;
    d.last_out_offset = outputOffset;
    d.last_out_count = outCount;
    return true;
}

int scale_offset_to_44100(int outputOffset, int rate) {
    if (rate == 11025) return outputOffset * 4;
    if (rate == 22050) return outputOffset * 2;
    return outputOffset;
}

// 8-bit PCM is signed here: the engine's scale table (0x1361B0) is built from (char)sample, and the file loader
// converts unsigned .wav data before it reaches a mixer.
inline float sample_to_float(int8_t v)  { return static_cast<float>(v) * (1.0f / 128.0f); }
inline float sample_to_float(int16_t v) { return static_cast<float>(v) * (1.0f / 32768.0f); }

template <class Sample, bool kStereo>
void capture_pcm(void* pChannel, const void* pData, int outputOffset,
                 int inputOffset, uint32_t rateScaleFix, int outCount)
{
    if (!g_engine || !pChannel || !pData || outCount <= 0) return;
    ensure_chunk_started();
    const int ch_idx = channel_index_from_ptr(pChannel);
    if (!should_capture(ch_idx, outputOffset, outCount)) return;

    const int n = std::min(outCount, kMaxChunk);
    const auto* src = static_cast<const Sample*>(pData);
    uint32_t frac = static_cast<uint32_t>(inputOffset) & kFixMask;
    int pos = 0;
    for (int i = 0; i < n; ++i) {
        if (kStereo) {
            g_tmp_l[i] = sample_to_float(src[pos * 2]);
            g_tmp_r[i] = sample_to_float(src[pos * 2 + 1]);
        } else {
            g_tmp_l[i] = sample_to_float(src[pos]);
        }
        const uint64_t next = static_cast<uint64_t>(frac) + rateScaleFix;
        pos += static_cast<int>(next >> kFixShift);
        frac = static_cast<uint32_t>(next & kFixMask);
    }

    UpsampleHistory& history = upsample_history(ch_idx, pChannel);
    const ChannelDescriptor desc = read_channel_descriptor(pChannel);
    const int offset = scale_offset_to_44100(outputOffset, g_current_mix_rate);
    const int up_n = upsample_to_44100(g_tmp_l, n, g_current_mix_rate, history.l, g_up_l, kMaxChunk);
    if (!kStereo) {
        g_engine->feed_channel_mono(ch_idx, desc, g_up_l, offset, up_n);
        return;
    }
    upsample_to_44100(g_tmp_r, n, g_current_mix_rate, history.r, g_up_r, kMaxChunk);
    if (desc.spatial_3d) {
        float wl = 0.5f, wr = 0.5f;
        stereo_fold_weights(pChannel, wl, wr);
        for (int i = 0; i < up_n; ++i) g_up_l[i] = g_up_l[i] * wl + g_up_r[i] * wr;
        g_engine->feed_channel_mono(ch_idx, desc, g_up_l, offset, up_n);
    } else {
        g_engine->feed_channel_stereo(ch_idx, desc, g_up_l, g_up_r, offset, up_n);
    }
}

// ---- Detours ----

// S_Update(origin, forward, right, up): called once per rendered frame, right after the frame was drawn, with that
// frame's camera. The pose goes to the audio thread before anything else happens, the channels' positions after the
// engine refreshed them. Neither depends on whether the engine paints audio this frame.
int __cdecl hook_s_update(const float* origin, const float* forward, const float* right, const float* up) {
    if (g_engine && origin && forward && right && up) {
        g_engine->update_listener(origin, forward, right, up);
    }
    if (g_engine && g_engine_base) {
        g_tweaks = g_engine->channel_tweaks();
        if ((g_frame_counter & 7u) == 0u) poll_cvars();
        ++g_frame_counter;
        poll_game_state();
        drain_messages();
    }
    const int result = orig_s_update(origin, forward, right, up);
    if (g_engine && g_engine_base && g_engine->is_enabled()) {
        g_engine->set_held(false); // PaintBegin starts the device again if nobody else has (0x115930)
        sync_channels(true);
        poll_player();
        poll_brush_entities();
        watch_sound_clock();
    }
    if (g_frame_callback) g_frame_callback();
    return result;
}

void __cdecl hook_s_paint_channels(int endtime) {
    orig_s_paint_channels(endtime);
    if (!g_engine || !g_engine_base || !g_engine->is_enabled()) return;
    sync_channels(false);
    const int painted = read_at<int>(g_engine_base, kRvaPaintedTime);
    const int sound_time = read_at<int>(g_engine_base, kRvaSoundTime);
    if (painted > sound_time) g_engine->set_engine_ahead(painted - sound_time);
}

// IAudioDevice::ClearBuffer: the engine silences what it already mixed (stopsound, level change, restart).
void __fastcall hook_clear_buffer(void* thisptr, void* edx) {
    if (g_engine && !g_clearing_own_buffer) g_engine->flush_audio();
    g_in_paint_chunk = false;
    orig_clear_buffer(thisptr, edx);
}

// The engine stops its DirectSound buffer, and with it its clock, while it writes a save game and while Windows' system
// menu is open. What was mixed ahead goes on afterwards from where it stopped.
void __cdecl hook_s_pause() {
    if (g_engine && g_engine->is_enabled()) g_engine->set_held(true);
    orig_s_pause();
}

void __cdecl hook_s_unpause() {
    orig_s_unpause();
    if (g_engine) g_engine->set_held(false);
}

void __cdecl hook_mix_channels(int endtime, int mixgroup, int sampleRate, int sampleRate2) {
    g_current_mix_rate = (sampleRate == 11025 || sampleRate == 22050 || sampleRate == 44100)
                             ? sampleRate : kDryRate;
    if (g_engine && g_engine->is_enabled() && g_engine_base) keep_audible_loops_mixing();
    orig_mix_channels(endtime, mixgroup, sampleRate, sampleRate2);
}

void __cdecl hook_s_free_channel(void* ch) {
    if (g_engine && ch) {
        const int idx = channel_index_from_ptr(ch);
        if (idx >= 0) g_engine->notify_channel_freed(idx);
    }
    orig_s_free_channel(ch);
}

void __fastcall hook_mix8_mono(void* thisptr, void* edx, void* ch, void* data,
                               int outOff, int inOff, uint32_t rateFix, int outCount, int tc) {
    if (g_engine && g_engine->is_enabled()) capture_pcm<int8_t, false>(ch, data, outOff, inOff, rateFix, outCount);
    else orig_mix8_mono(thisptr, edx, ch, data, outOff, inOff, rateFix, outCount, tc);
}

void __fastcall hook_mix8_stereo(void* thisptr, void* edx, void* ch, void* data,
                                 int outOff, int inOff, uint32_t rateFix, int outCount, int tc) {
    if (g_engine && g_engine->is_enabled()) capture_pcm<int8_t, true>(ch, data, outOff, inOff, rateFix, outCount);
    else orig_mix8_stereo(thisptr, edx, ch, data, outOff, inOff, rateFix, outCount, tc);
}

void __fastcall hook_mix16_mono(void* thisptr, void* edx, void* ch, void* data,
                                int outOff, int inOff, uint32_t rateFix, int outCount, int tc) {
    if (g_engine && g_engine->is_enabled()) capture_pcm<int16_t, false>(ch, data, outOff, inOff, rateFix, outCount);
    else orig_mix16_mono(thisptr, edx, ch, data, outOff, inOff, rateFix, outCount, tc);
}

void __fastcall hook_mix16_stereo(void* thisptr, void* edx, void* ch, void* data,
                                  int outOff, int inOff, uint32_t rateFix, int outCount, int tc) {
    if (g_engine && g_engine->is_enabled()) capture_pcm<int16_t, true>(ch, data, outOff, inOff, rateFix, outCount);
    else orig_mix16_stereo(thisptr, edx, ch, data, outOff, inOff, rateFix, outCount, tc);
}

void __fastcall hook_transfer_samples(void* thisptr, void* edx, int endtime) {
    if (g_engine && g_engine->is_enabled() && g_engine_base) {
        ensure_chunk_started(); // a chunk without any playing channel is silence, not a gap
        g_in_paint_chunk = false;
        const int frames = endtime - read_at<int>(g_engine_base, kRvaPaintedTime);
        const int sound_time = read_at<int>(g_engine_base, kRvaSoundTime);
        if (frames <= 0 || frames > kPaintBuffer || endtime - sound_time <= 0) {
            // After a hitch S_PaintChannels repaints the time that has already passed. DirectSound overwrites that
            // in its ring and never plays it; queueing it would delay everything after it for good.
            g_engine->discard_chunk();
        } else {
            g_engine->end_chunk(frames);
        }
        // Nothing is written to the DirectSound buffer: it plays silence while its clock keeps pacing
        // S_PaintChannels and lip-sync.
        return;
    }
    g_in_paint_chunk = false;
    orig_transfer(thisptr, edx, endtime);
}

struct HookSpec {
    const char* name;
    std::uintptr_t rva;
    const unsigned char* expected;
    std::size_t expected_len;
    void* detour;
    void** orig_out;
    bool required;
};

bool code_is(std::uintptr_t rva, const void* expected, std::size_t size) {
    return std::memcmp(g_engine_base + rva, expected, size) == 0;
}

// `mov`-like instruction: opcode bytes, the relocated address of `target`, and more bytes.
template <std::size_t N>
bool code_is_with_address(std::uintptr_t rva, const unsigned char (&expected)[N], std::size_t address_at, std::uintptr_t target) {
    unsigned char code[N];
    std::memcpy(code, expected, N);
    const uint32_t va = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(g_engine_base + target));
    std::memcpy(code + address_at, &va, 4);
    return code_is(rva, code, N);
}

} // namespace

bool install_engine_sound_hooks(const BLSE_HostAPI* host, SteamAudioEngine* engine) {
    if (g_hooks_installed) return true;
    if (!host || !engine || !BLSE_HOST_HAS(host, hook_function)) return false;

    HMODULE eng = GetModuleHandleW(L"engine.dll");
    if (!eng) {
        host->log("auspex_audio: engine.dll not loaded.");
        return false;
    }

    g_host = host;
    g_engine = engine;
    g_engine_base = reinterpret_cast<unsigned char*>(eng);

    // Exact verified prologues (whole instructions, no relative jumps/calls) from VTMB engine.dll.
    static const unsigned char kExpSUpdate[6]  = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    static const unsigned char kExpSPaint[5]   = { 0x83, 0xEC, 0x20, 0x53, 0x57 };
    static const unsigned char kExpMixCh[7]    = { 0x83, 0xEC, 0x08, 0x8B, 0x4C, 0x24, 0x0C };
    static const unsigned char kExpFreeCh[6]   = { 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
    static const unsigned char kExpMix[7]      = { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56, 0x57 };
    // TransferSamples and ClearBuffer start with `mov al, [surround flag]`, relocated with engine.dll.
    unsigned char exp_transfer[7] = { 0xA0, 0, 0, 0, 0, 0x84, 0xC0 };
    unsigned char exp_clear[8]    = { 0xA0, 0, 0, 0, 0, 0x83, 0xEC, 0x28 };
    const uint32_t surround_va = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(g_engine_base + kRvaSurroundFlag));
    std::memcpy(exp_transfer + 1, &surround_va, 4);
    std::memcpy(exp_clear + 1, &surround_va, 4);

    // The player's own sounds are told apart with GetViewEntity's global: only if that function is the known one.
    {
        unsigned char exp_view[6] = { 0xA1, 0, 0, 0, 0, 0xC3 };
        const uint32_t view_va = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(g_engine_base + kRvaViewEntity));
        std::memcpy(exp_view + 1, &view_va, 4);
        g_view_entity_known = std::memcmp(g_engine_base + kRvaGetViewEntity, exp_view, sizeof(exp_view)) == 0;
    }

    // Doors and speakers are read from the client's entity list: only if engine.dll reaches it the known way.
    {
        unsigned char exp_list[18] = { 0x8B, 0x0D, 0, 0, 0, 0, 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x01, 0x52, 0xFF, 0x50, 0x0C, 0x85, 0xC0 };
        const uint32_t list_va = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(g_engine_base + kRvaEntityList));
        std::memcpy(exp_list + 2, &list_va, 4);
        static const unsigned char kExpModelType[14] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x09, 0x8B, 0x80, 0x88, 0x00, 0x00, 0x00 };
        g_entities_known = std::memcmp(g_engine_base + kRvaGetSpatial, exp_list, sizeof(exp_list)) == 0 &&
                           std::memcmp(g_engine_base + kRvaGetModelType, kExpModelType, sizeof(kExpModelType)) == 0;
    }

    // Pause, focus and fade are read only if the code that uses them is the known one.
    {
        static const unsigned char kExpIsPaused[16] = { 0x8B, 0x0D, 0, 0, 0, 0, 0x85, 0xC9, 0x75, 0x03, 0x32, 0xC0, 0xC3, 0x8B, 0x01, 0xFF };
        g_pause_known = code_is_with_address(kRvaIsPaused, kExpIsPaused, 2, kRvaEngineClient) &&
                        read_at<unsigned char>(g_engine_base, kRvaIsPaused + 16) == 0x60 &&
                        read_at<unsigned char>(g_engine_base, kRvaIsPaused + 17) == 0x3C;
        static const unsigned char kExpActive[4] = { 0x8A, 0x41, 0x24, 0xC3 };
        const auto address = [](std::uintptr_t rva) { return reinterpret_cast<std::uintptr_t>(g_engine_base + rva); };
        g_focus_known = code_is(kRvaIsActiveApp, kExpActive, sizeof(kExpActive)) &&
                        read_at<uint32_t>(g_engine_base, kRvaGamePtr) == address(kRvaGame) &&
                        read_at<uint32_t>(g_engine_base, kRvaGameVtable + 0x28) == address(kRvaIsActiveApp);
        static const unsigned char kExpFade[6] = { 0xD9, 0x05, 0, 0, 0, 0 };
        g_fade_known = code_is_with_address(kRvaSoundFadeRead, kExpFade, 2, kRvaSoundFade);
    }

    // S_Pause and S_UnPause: mov ecx, [audio device]; test ecx, ecx; ...
    unsigned char exp_pause[6] = { 0x8B, 0x0D, 0, 0, 0, 0 };
    const uint32_t device_va = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(g_engine_base + kRvaAudioDevice));
    std::memcpy(exp_pause + 2, &device_va, 4);
    static const unsigned char kExpPauseEnd[9]   = { 0x85, 0xC9, 0x74, 0x05, 0x8B, 0x01, 0xFF, 0x60, 0x0C };
    static const unsigned char kExpUnPauseEnd[9] = { 0x85, 0xC9, 0x74, 0x05, 0x8B, 0x01, 0xFF, 0x60, 0x10 };
    const bool pause_functions_known = code_is(kRvaSPause + 6, kExpPauseEnd, sizeof(kExpPauseEnd)) &&
                                       code_is(kRvaSUnPause + 6, kExpUnPauseEnd, sizeof(kExpUnPauseEnd));

    const HookSpec specs[] = {
        { "S_Update",         kRvaSUpdate,        kExpSUpdate,  sizeof(kExpSUpdate),  reinterpret_cast<void*>(&hook_s_update),         reinterpret_cast<void**>(&orig_s_update), true },
        { "S_PaintChannels",  kRvaSPaintChannels, kExpSPaint,   sizeof(kExpSPaint),   reinterpret_cast<void*>(&hook_s_paint_channels), reinterpret_cast<void**>(&orig_s_paint_channels), true },
        { "MIX_MixChannels",  kRvaMixChannels,    kExpMixCh,    sizeof(kExpMixCh),    reinterpret_cast<void*>(&hook_mix_channels),     reinterpret_cast<void**>(&orig_mix_channels), true },
        { "S_FreeChannel",    kRvaSFreeChannel,   kExpFreeCh,   sizeof(kExpFreeCh),   reinterpret_cast<void*>(&hook_s_free_channel),   reinterpret_cast<void**>(&orig_s_free_channel), true },
        { "Mix8Mono",         kRvaMix8Mono,       kExpMix,      sizeof(kExpMix),      reinterpret_cast<void*>(&hook_mix8_mono),        reinterpret_cast<void**>(&orig_mix8_mono), true },
        { "Mix8Stereo",       kRvaMix8Stereo,     kExpMix,      sizeof(kExpMix),      reinterpret_cast<void*>(&hook_mix8_stereo),      reinterpret_cast<void**>(&orig_mix8_stereo), true },
        { "Mix16Mono",        kRvaMix16Mono,      kExpMix,      sizeof(kExpMix),      reinterpret_cast<void*>(&hook_mix16_mono),       reinterpret_cast<void**>(&orig_mix16_mono), true },
        { "Mix16Stereo",      kRvaMix16Stereo,    kExpMix,      sizeof(kExpMix),      reinterpret_cast<void*>(&hook_mix16_stereo),     reinterpret_cast<void**>(&orig_mix16_stereo), true },
        { "TransferSamples",  kRvaTransfer,       exp_transfer, sizeof(exp_transfer), reinterpret_cast<void*>(&hook_transfer_samples), reinterpret_cast<void**>(&orig_transfer), true },
        { "ClearBuffer",      kRvaClearBuffer,    exp_clear,    sizeof(exp_clear),    reinterpret_cast<void*>(&hook_clear_buffer),     reinterpret_cast<void**>(&orig_clear_buffer), true },
        { "S_Pause",          kRvaSPause,         exp_pause,    sizeof(exp_pause),    reinterpret_cast<void*>(&hook_s_pause),          reinterpret_cast<void**>(&orig_s_pause), false },
        { "S_UnPause",        kRvaSUnPause,       exp_pause,    sizeof(exp_pause),    reinterpret_cast<void*>(&hook_s_unpause),        reinterpret_cast<void**>(&orig_s_unpause), false },
    };
    constexpr std::size_t kSpecCount = sizeof(specs) / sizeof(specs[0]);

    int ok_count = 0;
    bool pausing = pause_functions_known;
    for (std::size_t i = 0; i < kSpecCount; ++i) {
        const auto& s = specs[i];
        if (!s.required && !pausing) continue;
        const int rc = host->hook_function(host->context, g_engine_base + s.rva,
                                           s.expected, s.expected_len, s.detour, s.orig_out);
        if (rc != BLSE_HOOK_OK && !s.required) {
            logf("auspex_audio: hook '%s' at RVA 0x%X not installed (rc=%d); sound does not wait while the game saves.",
                 s.name, static_cast<unsigned>(s.rva), rc);
            if (i > 0 && !specs[i - 1].required) host->unhook(host->context, g_engine_base + specs[i - 1].rva);
            pausing = false; // both or none
            continue;
        }
        if (rc != BLSE_HOOK_OK) {
            logf("auspex_audio: hook '%s' at RVA 0x%X failed (rc=%d); rolling back.",
                 s.name, static_cast<unsigned>(s.rva), rc);
            // Roll back any hooks installed so far.
            for (std::size_t j = 0; j < i; ++j) {
                host->unhook(host->context, g_engine_base + specs[j].rva);
            }
            g_engine_base = nullptr;
            return false;
        }
        ++ok_count;
    }

    g_hooks_installed = true;
    logf("auspex_audio: installed %d/%d engine.dll sound hooks cleanly (own sounds %s, doors %s, pause %s, focus %s, sound fade %s).",
         ok_count, static_cast<int>(kSpecCount), g_view_entity_known ? "centred" : "positional",
         g_entities_known ? "tracked" : "not tracked", g_pause_known ? "known" : "unknown",
         g_focus_known ? "known" : "unknown", g_fade_known ? "known" : "unknown");
    return true;
}

void remove_engine_sound_hooks(const BLSE_HostAPI* host) {
    if (!g_hooks_installed || !host || !g_engine_base || !BLSE_HOST_HAS(host, unhook)) return;
    const std::uintptr_t rvas[] = {
        kRvaSUpdate, kRvaSPaintChannels, kRvaMixChannels, kRvaSFreeChannel,
        kRvaMix8Mono, kRvaMix8Stereo, kRvaMix16Mono, kRvaMix16Stereo, kRvaTransfer, kRvaClearBuffer,
        kRvaSPause, kRvaSUnPause
    };
    for (std::uintptr_t rva : rvas) {
        host->unhook(host->context, g_engine_base + rva);
    }
    g_hooks_installed = false;
}

bool reinstall_engine_sound_hooks(const BLSE_HostAPI* host) {
    SteamAudioEngine* engine = g_engine;
    if (!engine) return false;
    g_hooks_installed = false;
    g_in_paint_chunk = false;
    g_frame_counter = 0;
    g_music_volume = 1.0f;
    g_effects_volume = 1.0f;
    g_clock = {};
    for (CaptureDedup& d : g_dedup) d = CaptureDedup{};
    for (UpsampleHistory& h : g_upsample) h = UpsampleHistory{};
    engine->set_held(false);
    engine->set_game_paused(false);
    engine->set_focused(true);
    engine->set_engine_ahead(0);
    if (install_engine_sound_hooks(host, engine)) return true;
    g_engine_base = nullptr; // nothing may reach the engine.dll that is gone
    return false;
}

void reset_brush_tracking() {
    g_brushes.clear();
    std::memset(g_brush_tracked, 0, sizeof(g_brush_tracked));
    g_brush_scan_next = 1;
    g_brush_faults = 0;
    if (g_engine) g_engine->update_brush_entities(nullptr, 0);
}

void set_frame_callback(void (*callback)()) { g_frame_callback = callback; }

void dump_channels(void (*print)(const char* line)) {
    if (!g_engine || !g_engine_base || !print) return;
    const std::vector<VoiceInfo> voices = g_engine->voice_infos();
    char line[512];
    char name[72];
    unsigned char raw[kChannelStride];
    int shown = 0;
    const int total = std::clamp(read_at<int>(g_engine_base, kRvaTotalChannels), 0, kMaxChannels);
    for (int i = 0; i < total; ++i) {
        const unsigned char* ch = g_engine_base + kRvaChannels + static_cast<std::size_t>(i) * kChannelStride;
        if (!copy_channel(ch, raw)) continue;
        const void* sfx = read_at<const void*>(raw, kChSfx);
        if (!sfx) continue;
        copy_name(sfx, name, sizeof(name));
        const VoiceInfo* v = nullptr;
        for (const VoiceInfo& info : voices) {
            if (info.slot == i) v = &info;
        }
        const float dist_mult = read_at<float>(raw, kChDistMult);
        const int* vol = reinterpret_cast<const int*>(raw + kChVolumes);
        std::snprintf(line, sizeof(line), "#%-3d %s ent=%d ch=%d '%s' vol=%d/%d/%d/%d master=%d dist_mult=%.6f%s%s\n",
                      i, !v ? "--" : (v->spatial_3d ? "3D" : "2D"),
                      read_at<int>(raw, kChEntNum), read_at<int>(raw, kChEntChannel), name,
                      vol[0], vol[1], vol[2], vol[3], read_at<int>(raw, kChMasterVol), dist_mult,
                      read_at<uint8_t>(raw, kChDry) != 0 ? " dry" : "",
                      read_at<uint8_t>(raw, kChSkipCollide) != 0 ? " skip_collide" : "");
        print(line);
        if (v) {
            std::snprintf(line, sizeof(line),
                          "     voice %d: dist=%.1f m gain=%.3f atten=%.3f occ=%.2f%s trans=%.2f/%.2f/%.2f reverb=%.3f%s",
                          v->voice, v->distance_m, v->master_gain, v->attenuation, v->occlusion,
                          v->no_occlusion ? "(off)" : "", v->transmission[0], v->transmission[1], v->transmission[2],
                          v->reverb_send, v->dialogue ? " dialogue" : (v->is_voice ? " is_voice" : ""));
            print(line);
            if (v->directional) {
                std::snprintf(line, sizeof(line), " directivity=%.2f", v->directivity);
                print(line);
            }
            if (v->weather > 0.0f) {
                std::snprintf(line, sizeof(line), " weather=%.2f (gain %.2f)", v->weather, v->weather_gain);
                print(line);
            } else if (v->weather_gain < 0.995f) {
                std::snprintf(line, sizeof(line), " rain=%.2f", v->weather_gain);
                print(line);
            }
            if (v->glass > 0.005f) {
                std::snprintf(line, sizeof(line), " glass=%.2f", v->glass);
                print(line);
            }
            if (v->recorded_db > -100.0f) {
                std::snprintf(line, sizeof(line), " recorded=%.1f dB", v->recorded_db);
                print(line);
            }
            if (std::fabs(v->level_db) > 0.05f) {
                std::snprintf(line, sizeof(line), " leveled=%+.1f dB", v->level_db);
                print(line);
            }
            if (v->outdoor_ambient) print(v->relocated ? " outdoor(moved to the street)" : " outdoor");
            print("\n");
        }
        ++shown;
    }
    for (const VoiceInfo& v : voices) {
        if (v.slot >= 0) continue;
        std::snprintf(line, sizeof(line), "     voice %d: %s, channel already freed, playing out (gain=%.3f dist=%.1f m)\n",
                      v.voice, v.spatial_3d ? "3D" : "2D", v.master_gain, v.distance_m);
        print(line);
    }
    std::snprintf(line, sizeof(line), "  %d channels, %u voices\n", shown, static_cast<unsigned>(voices.size()));
    print(line);
}

int tracked_brush_count() { return static_cast<int>(g_brushes.size()); }

bool engine_sound_hooks_installed() {
    return g_hooks_installed;
}

void clear_vanilla_audio_buffer() {
    if (!g_engine_base) return;
    g_clearing_own_buffer = true; // only DirectSound's copy, not what Steam Audio is playing
    __try {
        void* dev = *reinterpret_cast<void* const*>(g_engine_base + kRvaAudioDevice);
        auto* vtbl = dev ? *reinterpret_cast<void* const* const*>(dev) : nullptr;
        if (vtbl) {
            // IAudioDevice::ClearBuffer is vtable slot +0x34 (index 13).
            using ClearFn = void (__thiscall*)(void*);
            auto fn = reinterpret_cast<ClearFn>(vtbl[13]);
            if (fn) fn(dev);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Ignore if audio device is torn down or not yet initialized.
    }
    g_clearing_own_buffer = false;
}

} // namespace sa
