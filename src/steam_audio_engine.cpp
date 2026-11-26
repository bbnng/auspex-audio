#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "steam_audio_engine.h"

#include "device_watch.h"
#include "phonon_memory.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace sa {
namespace {

inline float soft_limit(float x) {
    if (!std::isfinite(x)) return 0.0f; // never to the player's ears
    // Transparent up to +-0.85, smooth tanh knee above to prevent digital clipping.
    if (x > 0.85f) {
        return 0.85f + 0.15f * std::tanh((x - 0.85f) / 0.15f);
    }
    if (x < -0.85f) {
        return -0.85f + 0.15f * std::tanh((x + 0.85f) / 0.15f);
    }
    return x;
}

void ma_data_callback(ma_device* dev, void* output, const void*, ma_uint32 frame_count) {
    auto* engine = static_cast<SteamAudioEngine*>(dev->pUserData);
    auto* out = static_cast<float*>(output);
    if (!engine) {
        std::memset(out, 0, frame_count * 2 * sizeof(float));
        return;
    }
    engine->count_callback();
    engine->pace_output(dev);
    engine->render_output(out, frame_count);
}

uint32_t now_us() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

IPLDirectEffectParams make_default_direct_params(float dist_meters, float attenuation) {
    IPLDirectEffectParams p{};
    p.flags = static_cast<IPLDirectEffectFlags>(
        IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
    p.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    p.distanceAttenuation = attenuation;
    // Gentle physical air absorption over distance.
    const float d = std::max(0.0f, dist_meters);
    p.airAbsorption[0] = std::exp(-0.0002f * d);
    p.airAbsorption[1] = std::exp(-0.0017f * d);
    p.airAbsorption[2] = std::exp(-0.0182f * d);
    p.directivity = 1.0f;
    p.occlusion = 1.0f;
    p.transmission[0] = 1.0f;
    p.transmission[1] = 1.0f;
    p.transmission[2] = 1.0f;
    return p;
}

// Smoothing time constants. Direction and position are not smoothed at all: a camera turn is complete in the next block
// (the binaural effect cross-fades between two directions by itself). Gains follow in ~10 ms; occlusion hides its 30 Hz
// ray steps; weather takes the time a person needs to walk through a door.
constexpr float kGainSeconds      = 0.0097f;
constexpr float kOcclusionSeconds = 0.0333f;
constexpr float kWeatherSeconds   = 0.4f;
constexpr float kDirectivitySeconds = 0.1f; // characters turn in steps of their own thinking rate (10 Hz)
constexpr float kSilentGain       = 1e-4f;

// Fades that are heard as such: losing the focus and device swaps (everything), the pause menu (the reverb).
constexpr float kGateSeconds     = 0.06f;
constexpr float kRoomDownSeconds = 0.35f;
constexpr float kRoomUpSeconds   = 0.03f;

// The device thread.
constexpr int kDevicePollMs    = 1000; // looks after the device that often even if Windows reports nothing
constexpr int kDeviceRetryMs   = 500;  // while there is none
constexpr int kDeviceSettleMs  = 150;  // Windows reports one change several times, and the device is not ready at once
constexpr uint64_t kDeviceDeadMs = 2500; // started, but no longer asking for sound (standby, driver trouble)
constexpr uint64_t kDeviceDeadMaxMs = 80000;
constexpr std::size_t kMaxSofaBytes = 128u << 20;

// The room. engine.dll's share for the room (ChannelDescriptor::reverb_gain) says how much of a sound excites it; a
// source next to the listener's head is heard much louder than its reflections, one across the room hardly is.
constexpr float kNearSend      = 0.35f, kFarSend = 1.0f; // at <= 1 m, at >= 8 m
constexpr float kNearSendVoice = 0.20f;
constexpr float kDialogueSend  = 0.15f; // a line spoken to the player: close, and engine.dll all but mutes its room too
constexpr float kOccludedSend  = 0.35f; // what is behind a wall excites the room on the other side of it
// Gain of the two reverb models, so that both sound alike (measured: hybrid -2.5 dB, parametric -18 dB of the input).
constexpr float kHybridGain     = 0.60f;
constexpr float kParametricGain = 1.0f;
constexpr float kHybridOverlap  = 0.25f;
constexpr float kReverbFault    = 32.0f; // 30 dB above full scale is no room

// Rain heard under a roof.
constexpr float kStreetExposure  = 0.55f;
constexpr float kCoveredGain     = 0.50f;
constexpr float kCoveredCutoffHz = 1300.0f;
constexpr float kOpenCutoffHz    = 20000.0f;

// Least that passes through walls (low, mid, high band), for sounds and for voices.
constexpr float kMinTransmission[3]   = { 0.12f, 0.06f, 0.025f };
constexpr float kVoiceTransmission[3] = { 0.45f, 0.34f, 0.20f };

// A conversation through a pane of glass (pawn shop, blood bank): direct sound of the low, mid and high band, and the
// room, which stays as quiet as in any conversation.
constexpr float kGlassBands[3] = { 0.75f, 0.45f, 0.20f };
constexpr float kGlassSend     = 0.70f;
constexpr float kGlassSeconds  = 0.12f;

// Rain around the listener covers what is far away. Nothing within kRainNear meters, all of it from kRainFar on.
constexpr float kRainNear = 2.5f, kRainFar = 18.0f;
constexpr float kRainLevel = 0.50f, kRainMid = 0.35f, kRainHigh = 0.75f; // how much is gone there in full rain
// Traffic and the rest of the city step back as a whole.
constexpr float kRainAmbient = 0.45f, kRainAmbientMid = 0.35f, kRainAmbientHigh = 0.70f;
constexpr float kRainAmbientHz = 3500.0f;
// It begins and ends slowly, and a drizzle on the edge of what counts does not switch.
constexpr float kRainUpSeconds = 1.2f, kRainDownSeconds = 2.5f;
constexpr float kRainOn = 0.08f, kRainOff = 0.04f;

// From this much open sky on the listener is out in the street (doorways see 10 %).
constexpr float kOutdoorExposure = 0.15f;

constexpr float kOcclusionRadius = 0.35f; // meters

// 0 at the level where the engine stops mixing a loop, 1 from twice that level (6 dB) on.
float loop_cull_window(float level) {
    const float t = std::clamp((level - kCullLevel) / kCullLevel, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float distance_send(float dist_m, bool is_voice) {
    const float near_send = is_voice ? kNearSendVoice : kNearSend;
    const float t = std::clamp((dist_m - 1.0f) / 7.0f, 0.0f, 1.0f);
    return near_send + (kFarSend - near_send) * t;
}

// How much of the outdoor rain arrives. The streets of sm_hub_1 see 55 to 85 % of the sky between the houses, its alleys
// about 25 %, doorways 10 %. From there on an open share of the sky lets its share of the energy in, so the level goes
// with the root.
float weather_openness(float exposure) {
    return std::sqrt(std::clamp(exposure / kStreetExposure, 0.0f, 1.0f));
}

float rain_distance(float dist_m) {
    const float t = std::clamp((dist_m - kRainNear) / (kRainFar - kRainNear), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t) * 0.5f + t * 0.5f;
}

// A level typed into the console or found in a file: anything not finite ("nan") keeps what there was.
float sane_level(float value, float previous) { return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : previous; }

float lowpass_coeff(float hz, float rate) { return 1.0f - std::exp(-6.2831853f * hz / rate); }

float weather_cutoff_coeff(float openness, float rate) {
    if (openness > 0.999f) return 1.0f;
    const float hz = kCoveredCutoffHz * std::pow(kOpenCutoffHz / kCoveredCutoffHz, std::clamp(openness, 0.0f, 1.0f));
    return 1.0f - std::exp(-6.2831853f * hz / rate);
}

constexpr float kTailSeconds = 0.011f; // HRTF filter tail after a voice's last dry frame
constexpr uint32_t kEndFadeFrames    = 128; // dry frames: voices that stop mid-waveform (culled, stopped, paused)
constexpr uint32_t kResumeFadeFrames = 512; // and the same loop coming back

// Alignment of the dry timeline with the engine's playback clock (dry frames). It decides when a sound is heard
// (lip-sync, like vanilla), never which direction it comes from.
constexpr int32_t kSurplusSkip = 4096;         // 93 ms late: jump
constexpr int32_t kSurplusHigh = 1024, kSurplusHighDone = 256;
constexpr int32_t kSurplusLow = -1536, kSurplusLowDone = -512;

// A brush counts as moved from here on (Source units, degrees).
constexpr float kMoveEpsilon = 0.05f;
// Every instance costs every ray a little: only the nearest brushes take part (a few more until they leave again), and
// buttons, bottles and the like are no obstacle at all.
constexpr std::size_t kMaxBrushes = 48, kMaxBrushesLeave = 56;
constexpr float kBrushRange = 50.0f, kBrushRangeLeave = 55.0f; // meters
constexpr float kMinBrushExtent = 0.6f;                        // meters, of the second longest side

} // namespace

int block_for_period(uint32_t period) {
    int best = 0;
    for (uint32_t n = 1; n <= period / kMinFrameSize; ++n) {
        if (period % n != 0) continue;
        const int f = static_cast<int>(period / n);
        if (f < kMinFrameSize || f > kMaxFrameSize) continue;
        if (best == 0 || std::abs(f - kDefaultFrameSize) < std::abs(best - kDefaultFrameSize)) best = f;
    }
    return best != 0 ? best : kDefaultFrameSize;
}

SteamAudioEngine::SteamAudioEngine() {
    a_listener_.origin = { 0.0f, 0.0f, 0.0f };
    a_listener_.ahead  = { 0.0f, 0.0f, -1.0f };
    a_listener_.right  = { 1.0f, 0.0f, 0.0f };
    a_listener_.up     = { 0.0f, 1.0f, 0.0f };
}

SteamAudioEngine::~SteamAudioEngine() {
    shutdown();
}

bool SteamAudioEngine::init(const EngineConfig& cfg, bool headless, std::string& out_error) {
    if (initialized_.load(std::memory_order_acquire)) return true;

    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg_ = cfg;
    }
    headless_ = headless;
    enabled_.store(cfg.enabled, std::memory_order_release);
    apply_settings(cfg);
    sky_exposure_.store(1.0f, std::memory_order_relaxed);
    focused_.store(true, std::memory_order_relaxed);
    game_paused_.store(false, std::memory_order_relaxed);
    held_.store(false, std::memory_order_relaxed);
    swap_mute_.store(false, std::memory_order_relaxed);

    bool ok = false;
    if (headless_) {
        rate_ = cfg.sample_rate == 48000 ? 48000 : kDryRate;
        frame_ = kDefaultFrameSize;
        ok = create_core(out_error) && create_dsp(cfg, out_error);
    } else {
        // The output device belongs to one thread from opening to closing, and so does the COM it needs: the game's main
        // thread is left as it is, and a device that takes its time never holds up a frame.
        {
            std::lock_guard<std::mutex> lk(ctl_mutex_);
            ctl_stop_ = ctl_device_event_ = ctl_reopen_ = false;
            ctl_init_ = 0;
        }
        device_thread_ = std::thread(&SteamAudioEngine::device_thread_main, this);
        std::unique_lock<std::mutex> lk(ctl_mutex_);
        ctl_cv_.wait(lk, [this] { return ctl_init_ != 0; });
        ok = ctl_init_ == 1;
        if (!ok) out_error = ctl_init_error_;
    }
    if (!ok) {
        shutdown();
        return false;
    }

    // Start background raytracing simulation thread (if not headless).
    stop_sim_.store(false, std::memory_order_release);
    if (!headless_) {
        sim_thread_ = std::thread(&SteamAudioEngine::sim_thread_main, this);
    }

    initialized_.store(true, std::memory_order_release);
    return true;
}

// The context and the (empty) scene: independent of the output rate, they live from init() to shutdown().
bool SteamAudioEngine::create_core(std::string& out_error) {
    phonon_memory_ = acquire_zeroed_phonon_memory(); // not even the context before this
    if (!phonon_memory_) post_message("phonon.dll's memory cannot be zeroed; its reverb may start with a fault.");
    IPLContextSettings ctx_cfg{};
    ctx_cfg.version = STEAMAUDIO_VERSION;
    ctx_cfg.simdLevel = IPL_SIMDLEVEL_SSE2; // Safe for 32-bit x86 builds
    IPLerror err = iplContextCreate(&ctx_cfg, &ipl_ctx_);
    if (err != IPL_STATUS_SUCCESS || !ipl_ctx_) {
        out_error = "iplContextCreate failed (error " + std::to_string(err) + ")";
        return false;
    }
    IPLSceneSettings scene_cfg{};
    scene_cfg.type = IPL_SCENETYPE_DEFAULT; // built-in CPU raytracer for x86
    err = iplSceneCreate(ipl_ctx_, &scene_cfg, &ipl_scene_);
    if (err != IPL_STATUS_SUCCESS || !ipl_scene_) {
        out_error = "iplSceneCreate failed (error " + std::to_string(err) + ")";
        return false;
    }
    return true;
}

// Custom SOFA if specified, otherwise (or if it cannot be loaded, with a warning) the built-in HRTF.
IPLHRTF SteamAudioEngine::create_hrtf(const EngineConfig& cfg, int rate, int frame, HrtfInfo& info) const {
    info = HrtfInfo{};
    IPLAudioSettings audio{};
    audio.samplingRate = rate;
    audio.frameSize = frame;
    IPLHRTFSettings settings{};
    settings.volume = 1.0f;
    settings.normType = IPL_HRTFNORMTYPE_NONE;
    IPLHRTF hrtf = nullptr;

    if (!cfg.sofa_file.empty()) {
        // Read here: Steam Audio opens files by their name in the system code page, and a mod folder can be anywhere.
        std::string data;
        if (cfg.sofa_path.empty()) {
            info.warning = "'" + cfg.sofa_file + "' was not found";
        } else if (!read_file(cfg.sofa_path, data, kMaxSofaBytes) || data.empty()) {
            info.warning = "'" + cfg.sofa_file + "' cannot be read";
        } else {
            settings.type = IPL_HRTFTYPE_SOFA;
            settings.sofaData = reinterpret_cast<const IPLuint8*>(data.data());
            settings.sofaDataSize = static_cast<int>(data.size());
            if (iplHRTFCreate(ipl_ctx_, &audio, &settings, &hrtf) == IPL_STATUS_SUCCESS && hrtf) {
                const std::size_t slash = cfg.sofa_path.find_last_of(L"\\/");
                info.name = narrow(slash == std::wstring::npos ? cfg.sofa_path : cfg.sofa_path.substr(slash + 1));
                return hrtf;
            }
            hrtf = nullptr;
            info.warning = "'" + cfg.sofa_file + "' is not a .sofa file Steam Audio can load";
        }
        settings.sofaData = nullptr;
        settings.sofaDataSize = 0;
    }
    settings.type = IPL_HRTFTYPE_DEFAULT;
    if (iplHRTFCreate(ipl_ctx_, &audio, &settings, &hrtf) != IPL_STATUS_SUCCESS) return nullptr;
    return hrtf;
}

// Everything that depends on rate_, frame_, the HRTF or the kind of reverb. Takes `hrtf` over if one is given.
bool SteamAudioEngine::create_dsp(const EngineConfig& cfg, std::string& out_error, IPLHRTF hrtf, const HrtfInfo* info) {
    const float block_seconds = static_cast<float>(frame_) / static_cast<float>(rate_);
    const auto alpha = [&](float seconds) { return 1.0f - std::exp(-block_seconds / seconds); };
    gain_alpha_ = alpha(kGainSeconds);
    occ_alpha_ = alpha(kOcclusionSeconds);
    weather_alpha_ = alpha(kWeatherSeconds);
    dir_alpha_ = alpha(kDirectivitySeconds);
    glass_alpha_ = alpha(kGlassSeconds);
    rain_up_alpha_ = alpha(kRainUpSeconds);
    rain_down_alpha_ = alpha(kRainDownSeconds);
    gate_step_ = block_seconds / kGateSeconds;
    room_down_step_ = block_seconds / kRoomDownSeconds;
    room_up_step_ = block_seconds / kRoomUpSeconds;
    tail_blocks_ = std::max(2, static_cast<int>(std::ceil(kTailSeconds / block_seconds)));
    reverb_tail_ = static_cast<int>(4.0f / block_seconds);
    surplus_window_ = std::max(1, static_cast<int>(0.5f / block_seconds));
    dry_per_block_ = static_cast<int32_t>(std::ceil(frame_ * (static_cast<double>(kDryRate) / rate_) * 1.005)) + 1;
    min_fill_ = dry_per_block_ + static_cast<int32_t>(kEndFadeFrames) + Resampler::kTaps;
    resampler_.init(frame_);

    HrtfInfo made;
    ipl_hrtf_ = hrtf ? hrtf : create_hrtf(cfg, rate_, frame_, made);
    if (!ipl_hrtf_) {
        out_error = "iplHRTFCreate failed";
        release_dsp();
        return false;
    }
    if (hrtf && info) made = *info;

    IPLAudioSettings audio_cfg{};
    audio_cfg.samplingRate = rate_;
    audio_cfg.frameSize = frame_;

    // The simulator, the shared reflection effect and its binaural decoder. If hybrid reverb cannot be created,
    // parametric reverb is used.
    if (!(cfg.reverb_mode == ReverbMode::kHybrid && create_reverb(cfg, ReverbMode::kHybrid, audio_cfg)) &&
        !create_reverb(cfg, ReverbMode::kParametric, audio_cfg)) {
        out_error = "failed to create the Steam Audio simulator and reflection effect";
        release_dsp();
        return false;
    }

    IPLAmbisonicsDecodeEffectSettings ambi_cfg{};
    ambi_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    ambi_cfg.hrtf = ipl_hrtf_;
    ambi_cfg.maxOrder = kAmbisonicsOrder;
    if (iplAmbisonicsDecodeEffectCreate(ipl_ctx_, &audio_cfg, &ambi_cfg, &ambi_decode_) != IPL_STATUS_SUCCESS || !ambi_decode_) {
        out_error = "iplAmbisonicsDecodeEffectCreate failed";
        release_dsp();
        return false;
    }

    // Per-voice Steam Audio effects and sources. Reflections run for one shared source only.
    IPLSourceSettings src_cfg{};
    src_cfg.flags = IPL_SIMULATIONFLAGS_DIRECT;
    IPLSourceSettings rev_cfg{};
    rev_cfg.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    IPLDirectEffectSettings dir_cfg{};
    dir_cfg.numChannels = 1;
    IPLBinauralEffectSettings bin_cfg{};
    bin_cfg.hrtf = ipl_hrtf_;
    for (Voice& v : voices_) {
        if (iplSourceCreate(ipl_sim_, &src_cfg, &v.source) != IPL_STATUS_SUCCESS ||
            iplDirectEffectCreate(ipl_ctx_, &audio_cfg, &dir_cfg, &v.direct_effect) != IPL_STATUS_SUCCESS ||
            iplBinauralEffectCreate(ipl_ctx_, &audio_cfg, &bin_cfg, &v.binaural_effect) != IPL_STATUS_SUCCESS) {
            out_error = "failed to create per-voice Steam Audio effects";
            release_dsp();
            return false;
        }
    }
    if (iplSourceCreate(ipl_sim_, &rev_cfg, &reverb_source_) != IPL_STATUS_SUCCESS) {
        out_error = "failed to create the reverb source";
        release_dsp();
        return false;
    }
    iplSourceAdd(reverb_source_, ipl_sim_);
    iplSimulatorCommit(ipl_sim_);

    for (auto* buf : { &scratch_dry_l_, &scratch_dry_r_, &scratch_mono_, &scratch_direct_, &scratch_bin_l_,
                       &scratch_bin_r_, &mix_l_, &mix_r_, &env_, &rev_send_, &rev_out_l_, &rev_out_r_ }) {
        buf->assign(static_cast<std::size_t>(frame_), 0.0f);
    }
    for (auto& ambi : rev_ambi_) ambi.assign(static_cast<std::size_t>(frame_), 0.0f);
    listen_l_.assign(512, 0.0f);
    listen_r_.assign(512, 0.0f);
    fetch_.assign(static_cast<std::size_t>(dry_per_block_) + 2u * Resampler::kTaps, 0.0f);
    carry_.assign(static_cast<std::size_t>(frame_) * 2u, 0.0f);
    reset_audio_state();

    pub_rate_.store(rate_, std::memory_order_relaxed);
    pub_frame_.store(frame_, std::memory_order_relaxed);
    pub_reverb_mode_.store(static_cast<int>(reverb_mode_), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        hrtf_info_ = made;
    }
    dsp_ready_ = true;
    return true;
}

// The sounds themselves (voices, timeline) and the scene stay.
void SteamAudioEngine::release_dsp() {
    dsp_ready_ = false;
    refl_valid_.store(false, std::memory_order_release);
    for (Voice& v : voices_) {
        if (v.source && v.in_simulator && ipl_sim_) iplSourceRemove(v.source, ipl_sim_);
        v.in_simulator = false;
        if (v.source)          { iplSourceRelease(&v.source); }
        if (v.direct_effect)   { iplDirectEffectRelease(&v.direct_effect); }
        if (v.binaural_effect) { iplBinauralEffectRelease(&v.binaural_effect); }
        v.source = nullptr;
        v.direct_effect = nullptr;
        v.binaural_effect = nullptr;
    }
    if (reverb_source_) {
        if (ipl_sim_) iplSourceRemove(reverb_source_, ipl_sim_);
        iplSourceRelease(&reverb_source_);
        reverb_source_ = nullptr;
    }
    if (ambi_decode_) { iplAmbisonicsDecodeEffectRelease(&ambi_decode_); }
    if (refl_effect_) { iplReflectionEffectRelease(&refl_effect_); }
    if (ipl_sim_)     { iplSimulatorRelease(&ipl_sim_); }
    if (ipl_hrtf_)    { iplHRTFRelease(&ipl_hrtf_); }
    ambi_decode_ = nullptr;
    refl_effect_ = nullptr;
    ipl_sim_ = nullptr;
    ipl_hrtf_ = nullptr;
}

bool SteamAudioEngine::rebuild_dsp(const EngineConfig& cfg, int rate, int frame, std::string& out_error, IPLHRTF hrtf,
                                   const HrtfInfo* info) {
    std::lock_guard<std::mutex> run_lk(sim_run_mutex_); // raytracing waits; loading a map does too
    std::string old_warning, new_warning;
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        old_warning = hrtf_info_.warning;
    }
    release_dsp();
    rate_ = rate;
    frame_ = frame;
    if (!create_dsp(cfg, out_error, hrtf, info)) return false;
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        new_warning = hrtf_info_.warning;
    }
    if (!new_warning.empty() && new_warning != old_warning) {
        post_message("HRTF " + new_warning + "; using the built-in one.");
    }
    scene_dirty_ = true;
    dsp_rebuilds_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The audio thread starts anew: it rejoins the engine's clock, and every sound starts from where it is now.
void SteamAudioEngine::reset_audio_state() {
    // There was no device for longer than the timeline holds: what is left in it is from back then.
    const uint32_t write = write_pos_.load(std::memory_order_acquire);
    if (write - read_pos_.load(std::memory_order_relaxed) > DryRing::kCapacity / 8 * 7) {
        read_pos_.store(write, std::memory_order_release);
    }
    carry_pos_ = carry_len_ = 0;
    a_starved_ = true;
    a_fade_in_ = false;
    a_held_ = false;
    a_skip_frames_ = 0;
    a_frac_ = 0.0;
    a_rate_step_ = a_surplus_blocks_ = a_reverb_tail_blocks_ = 0;
    a_refl_known_ = false;
    a_room_live_ = false;
    a_master_gain_ = -1.0f;
    a_bass_guard_.reset();
    a_limiter_.reset();
    for (Voice& v : voices_) {
        v.a_smooth_init = false;
        v.a_reset_fx = true;
        v.a_tail_blocks = 0;
    }
    if (refl_effect_) iplReflectionEffectReset(refl_effect_);
    if (ambi_decode_) iplAmbisonicsDecodeEffectReset(ambi_decode_);
    pacer_.reset();
}

// ---------------------------------------------------------------------------------------------------------------
// Device thread
// ---------------------------------------------------------------------------------------------------------------

void SteamAudioEngine::device_thread_main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        DefaultDeviceWatch watch;
        watch.start([this] {
            {
                std::lock_guard<std::mutex> lk(ctl_mutex_);
                ctl_device_event_ = true;
            }
            ctl_cv_.notify_all();
        });

        const EngineConfig cfg = config();
        std::string error;
        int rate = kDryRate, frame = kDefaultFrameSize;
        bool ok = open_device(cfg, rate, frame, error); // first: everything else runs at its rate
        if (ok) {
            rate_ = rate;
            frame_ = frame;
            ok = create_core(error) && create_dsp(cfg, error) && start_device(error);
        }
        {
            std::lock_guard<std::mutex> lk(ctl_mutex_);
            ctl_init_ = ok ? 1 : 2;
            ctl_init_error_ = error;
        }
        ctl_cv_.notify_all();
        if (ok) device_loop(watch);
        close_device();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void SteamAudioEngine::device_loop(DefaultDeviceWatch& watch) {
    for (;;) {
        bool event = false, reopen = false;
        {
            std::unique_lock<std::mutex> lk(ctl_mutex_);
            ctl_cv_.wait_for(lk, std::chrono::milliseconds(ma_dev_ ? kDevicePollMs : kDeviceRetryMs), [this] {
                return ctl_stop_ || ctl_device_event_ || rebuild_pending_.load(std::memory_order_relaxed);
            });
            if (ctl_stop_) return;
            event = ctl_device_event_;
            if (event) {
                ctl_cv_.wait_for(lk, std::chrono::milliseconds(kDeviceSettleMs), [this] { return ctl_stop_; });
                if (ctl_stop_) return;
                ctl_device_event_ = false;
                reopen = ctl_reopen_;
                ctl_reopen_ = false;
            }
        }
        check_device(watch, reopen);
        if (rebuild_pending_.load(std::memory_order_acquire)) apply_rebuild_request();
    }
}

void SteamAudioEngine::check_device(DefaultDeviceWatch& watch, bool reopen) {
    const std::wstring default_id = watch.default_id();
    const uint32_t callbacks = callbacks_.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    if (callbacks != seen_callbacks_) {
        seen_callbacks_ = callbacks;
        progress_ms_ = now;
        dead_after_ms_ = kDeviceDeadMs;
    }
    bool dead = false;
    const char* reason = reopen ? "asked to" : nullptr;
    if (!ma_dev_) reason = "an output device is there again";
    else if (ma_device_get_state(ma_dev_) != ma_device_state_started) reason = "the output device was removed, or its format changed";
    else if (!default_id.empty() && default_id != device_id_ && default_id != tolerated_id_) reason = "another default output device";
    else if (now - progress_ms_ > dead_after_ms_) {
        reason = "the output device stopped asking for sound";
        dead = true;
    }
    if (!reason) return;
    if (dead) dead_after_ms_ = std::min(dead_after_ms_ * 2, kDeviceDeadMaxMs);

    if (default_id.empty()) {
        if (ma_dev_) {
            close_device();
            post_message("no output device left; sound comes back as soon as there is one.");
        }
        return;
    }
    reopen_device(reason);
    tolerated_id_ = (ma_dev_ && device_id_ != default_id) ? default_id : std::wstring();
}

void SteamAudioEngine::reopen_device(const char* reason) {
    const uint32_t old_rate = dev_rate_.load(std::memory_order_relaxed);
    fade_out_and_wait();
    close_device();

    const EngineConfig cfg = config();
    std::string error;
    int rate = kDryRate, frame = kDefaultFrameSize;
    bool ok = open_device(cfg, rate, frame, error);
    bool rebuilt = false;
    if (ok) {
        if (!dsp_ready_ || rate != rate_ || frame != frame_) {
            rebuilt = true;
            ok = rebuild_dsp(cfg, rate, frame, error);
        } else {
            reset_audio_state();
        }
    }
    swap_mute_.store(false, std::memory_order_relaxed);
    ok = ok && start_device(error);
    if (!ok) {
        close_device();
        if (!open_failed_) post_message(std::string("the output device cannot be opened (") + error + "); trying again.");
        open_failed_ = true;
        return;
    }
    open_failed_ = false;
    device_changes_.fetch_add(1, std::memory_order_relaxed);
    char text[512];
    std::string name;
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        name = device_name_;
    }
    std::snprintf(text, sizeof(text), "output device opened again (%s): '%.120s', %u Hz (was %u), period %u of %u frames%s.", reason,
                  name.c_str(), dev_rate_.load(std::memory_order_relaxed), old_rate, dev_period_.load(std::memory_order_relaxed),
                  dev_buffer_.load(std::memory_order_relaxed), rebuilt ? ", Steam Audio rebuilt for it" : "");
    post_message(text);
}

void SteamAudioEngine::apply_rebuild_request() {
    EngineConfig next = config();
    const EngineConfig before = next;
    bool want_hrtf = false, want_mode = false;
    {
        std::lock_guard<std::mutex> lk(ctl_mutex_);
        want_hrtf = ctl_want_hrtf_;
        want_mode = ctl_want_mode_;
        if (want_hrtf) {
            next.sofa_file = ctl_sofa_file_;
            next.sofa_path = ctl_sofa_path_;
        }
        if (want_mode) next.reverb_mode = ctl_mode_;
        ctl_want_hrtf_ = ctl_want_mode_ = false;
    }
    const auto done = [&](const std::string& text) {
        bool more = false;
        {
            std::lock_guard<std::mutex> lk(ctl_mutex_);
            more = ctl_want_hrtf_ || ctl_want_mode_;
            rebuild_pending_.store(more, std::memory_order_release);
        }
        if (!text.empty()) post_message(text);
    };

    // The HRTF first, while everything still plays: a file that cannot be loaded must not interrupt anything.
    IPLHRTF hrtf = nullptr;
    HrtfInfo info;
    if (want_hrtf && dsp_ready_) {
        hrtf = create_hrtf(next, rate_, frame_, info);
        if (!hrtf || !info.warning.empty()) {
            if (hrtf) iplHRTFRelease(&hrtf);
            post_message("HRTF unchanged: " + (info.warning.empty() ? std::string("it cannot be created") : info.warning) + ".");
            hrtf = nullptr;
            want_hrtf = false;
            next.sofa_file = before.sofa_file;
            next.sofa_path = before.sofa_path;
        }
    }
    if (!want_hrtf && (!want_mode || (next.reverb_mode == before.reverb_mode && dsp_ready_))) {
        done(want_mode ? "reverb mode unchanged." : "");
        return;
    }

    const bool was_running = device_running();
    fade_out_and_wait();
    if (was_running) ma_device_stop(ma_dev_);
    std::string error;
    bool ok = rebuild_dsp(next, rate_, frame_, error, hrtf, &info);
    if (!ok) {
        std::string again;
        next = before;
        if (!rebuild_dsp(before, rate_, frame_, again)) error += "; the old settings cannot be restored either";
    }
    if (ok) {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg_.sofa_file = next.sofa_file;
        cfg_.sofa_path = next.sofa_path;
        cfg_.reverb_mode = next.reverb_mode;
    }
    swap_mute_.store(false, std::memory_order_relaxed);
    if (was_running && dsp_ready_) {
        std::string start_error;
        if (!start_device(start_error)) close_device(); // the next look at the device opens it again
    } else if (ma_dev_ && !dsp_ready_) {
        close_device();
    }
    if (!ok) {
        done("settings not applied (" + error + ").");
        return;
    }
    std::string text;
    if (want_hrtf) {
        std::lock_guard<std::mutex> lk(status_mutex_); // also when create_dsp made it itself (no DSP before)
        text = "HRTF is now '" + hrtf_info_.name + "'.";
    }
    if (want_mode) {
        const bool hybrid = reverb_mode_ == ReverbMode::kHybrid;
        if (!text.empty()) text += ' ';
        text += std::string("Reverb mode is now ") + (hybrid ? "hybrid." : "parametric.");
        if (!hybrid && next.reverb_mode == ReverbMode::kHybrid) text += " (Hybrid reverb cannot be set up.)";
    }
    done(text);
}

bool SteamAudioEngine::open_device(const EngineConfig& cfg, int& out_rate, int& out_frame, std::string& out_error) {
    ma_dev_ = new ma_device{};
    ma_device_config dcfg = ma_device_config_init(ma_device_type_playback);
    dcfg.playback.format   = ma_format_f32;
    dcfg.playback.channels = 2;
    // The device's own rate and the smallest period it offers: Windows 10's low-latency shared mode (IAudioClient3) is
    // only granted without conversion. Callbacks of any size come straight from WASAPI: miniaudio's fixed-size mode
    // would put a block that was rendered one callback earlier in between.
    dcfg.sampleRate        = static_cast<ma_uint32>(cfg.sample_rate == 44100 || cfg.sample_rate == 48000 ? cfg.sample_rate : 0);
    dcfg.periodSizeInFrames = 1;
    dcfg.periods           = 2;
    dcfg.performanceProfile = ma_performance_profile_low_latency;
    dcfg.noFixedSizedCallback = MA_TRUE;
    dcfg.noClip            = MA_TRUE; // the output is already limited
    dcfg.wasapi.noAutoConvertSRC = MA_TRUE;
    // miniaudio would reroute by itself, inside Windows' notification callback: it leaks the old stream, keeps the old
    // rate (and then resamples linearly), does it once per device role, and starts a null stream if opening the new device
    // fails at that moment. The device thread does it instead.
    dcfg.wasapi.noAutoStreamRouting = MA_TRUE;
    dcfg.wasapi.usage      = ma_wasapi_usage_games; // MMCSS real-time priority for the callback thread
    dcfg.dataCallback      = ma_data_callback;
    dcfg.pUserData         = this;

    ma_result mr = ma_device_init(nullptr, &dcfg, ma_dev_);
    if (mr == MA_SUCCESS && ma_dev_->sampleRate != 44100 && ma_dev_->sampleRate != 48000) {
        // Steam Audio's HRTF exists at 44.1 and 48 kHz. miniaudio converts from there to 96 or 192 kHz.
        ma_device_uninit(ma_dev_);
        dcfg.sampleRate = 48000;
        mr = ma_device_init(nullptr, &dcfg, ma_dev_);
    }
    if (mr != MA_SUCCESS) {
        delete ma_dev_;
        ma_dev_ = nullptr;
        out_error = "ma_device_init failed (ma_result " + std::to_string(mr) + ")";
        return false;
    }
    out_rate = static_cast<int>(ma_dev_->sampleRate);
    out_frame = ma_dev_->playback.internalSampleRate == ma_dev_->sampleRate
                    ? block_for_period(ma_dev_->playback.internalPeriodSizeInFrames)
                    : kDefaultFrameSize;
    device_id_ = ma_dev_->playback.id.wasapi;
    dev_rate_.store(ma_dev_->playback.internalSampleRate, std::memory_order_relaxed);
    dev_period_.store(ma_dev_->playback.internalPeriodSizeInFrames, std::memory_order_relaxed);
    dev_buffer_.store(ma_dev_->wasapi.actualBufferSizeInFramesPlayback, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(status_mutex_);
    device_name_ = ma_dev_->playback.name;
    return true;
}

bool SteamAudioEngine::start_device(std::string& out_error) {
    const ma_result mr = ma_device_start(ma_dev_);
    if (mr != MA_SUCCESS) {
        out_error = "ma_device_start failed (ma_result " + std::to_string(mr) + ")";
        return false;
    }
    progress_ms_ = GetTickCount64();
    seen_callbacks_ = callbacks_.load(std::memory_order_relaxed);
    if (dead_after_ms_ == 0) dead_after_ms_ = kDeviceDeadMs;
    device_started_.store(true, std::memory_order_relaxed);
    return true;
}

bool SteamAudioEngine::device_running() const {
    return ma_dev_ && ma_device_get_state(ma_dev_) == ma_device_state_started;
}

// Nothing is cut off mid-waveform when the callback stops. A device that is gone already does not fade.
void SteamAudioEngine::fade_out_and_wait() {
    if (!device_running()) return;
    swap_mute_.store(true, std::memory_order_relaxed);
    const uint64_t until = GetTickCount64() + 200;
    while (!gate_closed_.load(std::memory_order_acquire) && GetTickCount64() < until) Sleep(5);
}

void SteamAudioEngine::close_device() {
    device_started_.store(false, std::memory_order_relaxed);
    if (!ma_dev_) return;
    ma_device_uninit(ma_dev_); // the callback has returned for good after this
    delete ma_dev_;
    ma_dev_ = nullptr;
    device_id_.clear();
}

void SteamAudioEngine::post_message(const std::string& text) {
    std::lock_guard<std::mutex> lk(msg_mutex_);
    if (messages_.size() >= 32) messages_.pop_front();
    messages_.push_back(text);
    has_message_.store(true, std::memory_order_release);
}

bool SteamAudioEngine::pop_message(std::string& out) {
    if (!has_message_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(msg_mutex_);
    if (messages_.empty()) return false;
    out = std::move(messages_.front());
    messages_.pop_front();
    has_message_.store(!messages_.empty(), std::memory_order_release);
    return true;
}

void SteamAudioEngine::reopen_output() {
    {
        std::lock_guard<std::mutex> lk(ctl_mutex_);
        ctl_device_event_ = ctl_reopen_ = true;
    }
    ctl_cv_.notify_all();
}

void SteamAudioEngine::request_reverb_mode(ReverbMode mode) {
    {
        std::lock_guard<std::mutex> lk(ctl_mutex_);
        ctl_want_mode_ = true;
        ctl_mode_ = mode;
        rebuild_pending_.store(true, std::memory_order_release);
    }
    ctl_cv_.notify_all();
    if (headless_) apply_rebuild_request();
}

void SteamAudioEngine::request_hrtf(const std::string& sofa_file, const std::wstring& sofa_path) {
    {
        std::lock_guard<std::mutex> lk(ctl_mutex_);
        ctl_want_hrtf_ = true;
        ctl_sofa_file_ = sofa_file;
        ctl_sofa_path_ = sofa_path;
        rebuild_pending_.store(true, std::memory_order_release);
    }
    ctl_cv_.notify_all();
    if (headless_) apply_rebuild_request();
}

bool SteamAudioEngine::change_output_sync(int rate, uint32_t period, std::string& out_error) {
    if (!headless_) return false;
    const int frame = block_for_period(period);
    if (dsp_ready_ && rate == rate_ && frame == frame_) {
        reset_audio_state();
        return true;
    }
    return rebuild_dsp(config(), rate, frame, out_error);
}

// The simulator and the shared reflection effect. They have to agree on the kind of reverb.
bool SteamAudioEngine::create_reverb(const EngineConfig& cfg, ReverbMode mode, const IPLAudioSettings& audio) {
    const bool hybrid = mode == ReverbMode::kHybrid;
    const float duration = std::clamp(cfg.reverb_duration, 0.5f, 3.0f);
    // The convolution's cost follows the length the effect is created with, not the simulated duration.
    early_samples_ = static_cast<int>(std::clamp(cfg.early_reflections, 0.1f, 0.8f * duration) * static_cast<float>(rate_));

    IPLSimulationSettings sim_cfg{};
    sim_cfg.flags = static_cast<IPLSimulationFlags>(
        IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    sim_cfg.sceneType = IPL_SCENETYPE_DEFAULT;
    sim_cfg.reflectionType = hybrid ? IPL_REFLECTIONEFFECTTYPE_HYBRID : IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    sim_cfg.maxNumOcclusionSamples = std::clamp(cfg.occlusion_samples, 4, 64);
    sim_cfg.maxNumRays = std::clamp(cfg.reflection_rays, 256, 4096);
    sim_cfg.numDiffuseSamples = 16;
    sim_cfg.maxDuration = duration;
    sim_cfg.maxOrder = kAmbisonicsOrder;
    sim_cfg.maxNumSources = kMaxVoices + 1;
    sim_cfg.numThreads = 2;
    sim_cfg.samplingRate = audio.samplingRate;
    sim_cfg.frameSize = audio.frameSize;

    IPLReflectionEffectSettings refl_cfg{};
    refl_cfg.type = sim_cfg.reflectionType;
    refl_cfg.irSize = hybrid ? early_samples_ : static_cast<IPLint32>(duration * static_cast<float>(rate_));
    refl_cfg.numChannels = kAmbisonicsChannels;

    IPLAudioSettings audio_cfg = audio;
    if (iplSimulatorCreate(ipl_ctx_, &sim_cfg, &ipl_sim_) != IPL_STATUS_SUCCESS || !ipl_sim_ ||
        iplReflectionEffectCreate(ipl_ctx_, &audio_cfg, &refl_cfg, &refl_effect_) != IPL_STATUS_SUCCESS || !refl_effect_) {
        if (refl_effect_) iplReflectionEffectRelease(&refl_effect_);
        if (ipl_sim_) iplSimulatorRelease(&ipl_sim_);
        refl_effect_ = nullptr;
        ipl_sim_ = nullptr;
        return false;
    }
    iplSimulatorSetScene(ipl_sim_, ipl_scene_);
    iplSimulatorCommit(ipl_sim_);
    reverb_mode_ = mode;
    return true;
}

void SteamAudioEngine::shutdown() {
    initialized_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(sim_wake_mutex_);
        stop_sim_.store(true, std::memory_order_release);
    }
    sim_wake_.notify_all();
    if (sim_thread_.joinable()) {
        sim_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(ctl_mutex_);
        ctl_stop_ = true;
    }
    ctl_cv_.notify_all();
    if (device_thread_.joinable()) {
        device_thread_.join(); // closes the device
    }

    release_dsp();
    for (Voice& v : voices_) v.state.store(kVoiceFree, std::memory_order_release);
    for (Slot& slot : slots_) slot = Slot{};
    rebuild_pending_.store(false, std::memory_order_relaxed);

    release_brush_instances();
    if (ipl_scene_)   { iplSceneCommit(ipl_scene_); }
    release_sub_scenes();
    brush_models_.clear();
    brush_materials_.clear();
    sky_.clear();
    glass_.clear();
    {
        std::lock_guard<std::mutex> lk(brush_mutex_);
        brush_states_.clear();
        brush_infos_.clear();
        brush_dirty_ = false;
    }
    if (ipl_mesh_ && ipl_scene_) {
        iplStaticMeshRemove(ipl_mesh_, ipl_scene_);
        iplStaticMeshRelease(&ipl_mesh_);
    } else if (ipl_mesh_) {
        iplStaticMeshRelease(&ipl_mesh_);
    }
    ipl_mesh_ = nullptr;
    if (ipl_scene_) { iplSceneRelease(&ipl_scene_); }
    if (ipl_ctx_)   { iplContextRelease(&ipl_ctx_); }
    ipl_scene_ = nullptr;
    ipl_ctx_ = nullptr;
    if (phonon_memory_) release_zeroed_phonon_memory();
    phonon_memory_ = false;

    std::lock_guard<std::mutex> lk(scene_info_mutex_);
    scene_tri_count_ = 0;
    brush_model_count_ = 0;
    glass_tri_count_ = 0;
    has_sky_ = false;
    loaded_map_.clear();
}

void SteamAudioEngine::set_enabled(bool on) {
    if (enabled_.exchange(on, std::memory_order_acq_rel) == on) return;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg_.enabled = on;
    }
    flush_audio(); // fades out what is playing, and never replays stale audio after a later sa_enable 1
}

EngineConfig SteamAudioEngine::config() const {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    return cfg_;
}

void SteamAudioEngine::set_master_volume(float vol) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.master_volume = sane_level(vol, cfg_.master_volume);
    master_volume_.store(cfg_.master_volume, std::memory_order_relaxed);
}

void SteamAudioEngine::set_hrtf_enabled(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.hrtf_enabled = on;
    hrtf_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_occlusion_enabled(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.occlusion_enabled = on;
    occlusion_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_reverb_enabled(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.reverb_enabled = on;
    reverb_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_reverb_wet(float wet) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.reverb_wet = sane_level(wet, cfg_.reverb_wet);
    reverb_wet_.store(cfg_.reverb_wet, std::memory_order_relaxed);
}

void SteamAudioEngine::set_dynamic_doors(bool on) {
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg_.dynamic_doors = on;
    }
    std::lock_guard<std::mutex> lk(brush_mutex_);
    brush_dirty_ = true;
}

void SteamAudioEngine::set_weather_occlusion(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.weather_occlusion = on;
    weather_occlusion_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_weather_volume(float volume) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.weather_volume = sane_level(volume, cfg_.weather_volume);
    weather_volume_.store(cfg_.weather_volume, std::memory_order_relaxed);
}

void SteamAudioEngine::set_rain_masking(float strength) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.rain_masking = sane_level(strength, cfg_.rain_masking);
    rain_masking_.store(cfg_.rain_masking, std::memory_order_relaxed);
}

void SteamAudioEngine::set_dialogue_glass(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.dialogue_glass = on;
    glass_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_tweaks(const EngineConfig& cfg) {
    set_dialogue_glass(cfg.dialogue_glass);
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.fix_npc_barks = cfg.fix_npc_barks;
    cfg_.level_voices = cfg.level_voices;
    cfg_.tame_loud_sounds = cfg.tame_loud_sounds;
    cfg_.fix_ambient_directions = cfg.fix_ambient_directions;
    cfg_.filter_heels_ambients = cfg.filter_heels_ambients;
    cfg_.filter_hoof_ambient = cfg.filter_hoof_ambient;
    tame_loud_.store(cfg.tame_loud_sounds, std::memory_order_relaxed);
}

ChannelTweaks SteamAudioEngine::channel_tweaks() const {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    ChannelTweaks t;
    t.npc_barks = cfg_.fix_npc_barks;
    t.level_voices = cfg_.level_voices;
    t.tame_loud = cfg_.tame_loud_sounds;
    t.ambient_directions = cfg_.fix_ambient_directions;
    t.no_heels = cfg_.filter_heels_ambients;
    t.no_hoofs = cfg_.filter_hoof_ambient;
    return t;
}

void SteamAudioEngine::set_voice_directivity(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.voice_directivity = on;
    directivity_on_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::set_mute_in_background(bool on) {
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    cfg_.mute_in_background = on;
    mute_in_background_.store(on, std::memory_order_relaxed);
}

void SteamAudioEngine::apply_settings(const EngineConfig& cfg) {
    set_master_volume(cfg.master_volume);
    set_mute_in_background(cfg.mute_in_background);
    set_hrtf_enabled(cfg.hrtf_enabled);
    set_occlusion_enabled(cfg.occlusion_enabled);
    set_reverb_enabled(cfg.reverb_enabled);
    set_reverb_wet(cfg.reverb_wet);
    set_voice_directivity(cfg.voice_directivity);
    set_weather_occlusion(cfg.weather_occlusion);
    set_weather_volume(cfg.weather_volume);
    set_rain_masking(cfg.rain_masking);
    set_tweaks(cfg);
    set_dynamic_doors(cfg.dynamic_doors);
}

void SteamAudioEngine::set_focused(bool focused) { focused_.store(focused, std::memory_order_relaxed); }
void SteamAudioEngine::set_game_paused(bool paused) { game_paused_.store(paused, std::memory_order_relaxed); }
void SteamAudioEngine::set_held(bool held) { held_.store(held, std::memory_order_relaxed); }

bool SteamAudioEngine::load_scene(const AcousticMeshData& mesh, std::string& out_summary) {
    if (!initialized_.load(std::memory_order_acquire) || !ipl_ctx_ || !ipl_scene_) {
        out_summary = "Steam Audio not initialized";
        return false;
    }
    if (mesh.empty() || mesh.materials.empty()) {
        out_summary = "acoustic mesh empty";
        return false;
    }

    IPLStaticMeshSettings mcfg{};
    mcfg.numVertices = static_cast<IPLint32>(mesh.vertices.size());
    mcfg.numTriangles = static_cast<IPLint32>(mesh.triangles.size());
    mcfg.numMaterials = static_cast<IPLint32>(mesh.materials.size());
    mcfg.vertices = const_cast<IPLVector3*>(mesh.vertices.data());
    mcfg.triangles = const_cast<IPLTriangle*>(mesh.triangles.data());
    mcfg.materialIndices = const_cast<IPLint32*>(mesh.material_indices.data());
    mcfg.materials = const_cast<IPLMaterial*>(mesh.materials.data());

    IPLStaticMesh new_mesh = nullptr;
    const IPLerror err = iplStaticMeshCreate(ipl_scene_, &mcfg, &new_mesh);
    if (err != IPL_STATUS_SUCCESS || !new_mesh) {
        out_summary = "iplStaticMeshCreate failed (error " + std::to_string(err) + ")";
        return false;
    }

    SkyProbe sky;
    sky.build(mesh);
    GlassProbe glass;
    glass.build(mesh);
    {
        // Steam Audio forbids committing while a simulation runs.
        std::lock_guard<std::mutex> run_lk(sim_run_mutex_);
        std::lock_guard<std::mutex> lk(scene_info_mutex_);
        release_brush_instances();
        brush_models_ = mesh.submodels;
        brush_materials_ = mesh.materials;
        sky_ = std::move(sky);
        sky_countdown_ = 0;
        glass_ = std::move(glass);
        for (Voice& v : voices_) v.s_placed = 0;
        {
            std::lock_guard<std::mutex> brush_lk(brush_mutex_);
            brush_dirty_ = true; // what is already known about this map's doors
        }
        if (ipl_mesh_) {
            iplStaticMeshRemove(ipl_mesh_, ipl_scene_);
            iplStaticMeshRelease(&ipl_mesh_);
        }
        ipl_mesh_ = new_mesh;
        iplStaticMeshAdd(ipl_mesh_, ipl_scene_);
        iplSceneCommit(ipl_scene_);
        if (ipl_sim_) {
            iplSimulatorSetScene(ipl_sim_, ipl_scene_);
            iplSimulatorCommit(ipl_sim_);
        }
        release_sub_scenes(); // after the commit: nothing refers to them any more
        scene_tri_count_ = static_cast<uint32_t>(mesh.triangles.size());
        brush_model_count_ = static_cast<uint32_t>(mesh.submodels.size());
        glass_tri_count_ = static_cast<uint32_t>(mesh.glass_triangles.size());
        has_sky_ = sky_.has_sky();
        loaded_map_ = mesh.map_name;
        scene_dirty_ = true;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Steam Audio scene ready: '%s' (%u tris, %u verts, %u mats, %u brush models, %u sky tris)",
                  mesh.map_name.c_str(),
                  static_cast<unsigned>(mesh.triangles.size()),
                  static_cast<unsigned>(mesh.vertices.size()),
                  static_cast<unsigned>(mesh.materials.size()),
                  static_cast<unsigned>(mesh.submodels.size()),
                  static_cast<unsigned>(mesh.sky_triangles.size()));
    out_summary = buf;
    return true;
}

void SteamAudioEngine::unload_scene() {
    if (!ipl_scene_) return;
    std::lock_guard<std::mutex> run_lk(sim_run_mutex_);
    std::lock_guard<std::mutex> lk(scene_info_mutex_);
    release_brush_instances();
    if (ipl_mesh_) {
        iplStaticMeshRemove(ipl_mesh_, ipl_scene_);
        iplStaticMeshRelease(&ipl_mesh_);
    }
    iplSceneCommit(ipl_scene_);
    if (ipl_sim_) {
        iplSimulatorSetScene(ipl_sim_, ipl_scene_);
        iplSimulatorCommit(ipl_sim_);
    }
    release_sub_scenes(); // after the commit: nothing refers to them any more
    brush_models_.clear();
    brush_materials_.clear();
    sky_.clear();
    glass_.clear();
    sky_exposure_.store(1.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> brush_lk(brush_mutex_);
        brush_states_.clear();
        brush_infos_.clear();
        brush_dirty_ = false;
    }
    scene_tri_count_ = 0;
    brush_model_count_ = 0;
    has_sky_ = false;
    glass_tri_count_ = 0;
    loaded_map_.clear();
    refl_valid_.store(false, std::memory_order_release);
    // Generation 0 is never used by a voice: every voice is unoccluded until it is simulated again.
    for (Voice& v : voices_) {
        v.sim.store(SimDirect{});
        v.place.store(SimPlace{});
        v.s_placed = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Game main thread
// ---------------------------------------------------------------------------------------------------------------

void SteamAudioEngine::update_listener(
    const float origin[3],
    const float forward[3],
    const float right[3],
    const float up[3])
{
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) sum += origin[i] + forward[i] + right[i] + up[i];
    if (!std::isfinite(sum)) return;
    const float fwd_sq = forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    if (fwd_sq < 0.25f) return; // vec3_origin: no view is being rendered

    ListenerPose pose{};
    pose.cs = make_ipl_listener(origin, forward, right, up);
    pose.stamp_us = now_us();
    listener_pub_.store(pose);
}

void SteamAudioEngine::update_player_head(const float position[3]) {
    PlayerHead head{};
    head.known = position && std::isfinite(position[0] + position[1] + position[2]);
    if (head.known) head.position = source_pos_to_ipl(position);
    player_pub_.store(head);
}

void SteamAudioEngine::publish_params(Voice& v, const ChannelDescriptor& desc) {
    VoiceParams p{};
    p.origin[0] = desc.origin[0];
    p.origin[1] = desc.origin[1];
    p.origin[2] = desc.origin[2];
    p.dist_mult = desc.dist_mult;
    p.radius = desc.radius;
    p.master_gain = std::clamp(desc.master_gain, 0.0f, 1.5f);
    p.loop_master = std::clamp(desc.loop_master, 0.0f, 1.0f);
    p.reverb_gain = std::clamp(desc.reverb_gain, 0.0f, 1.0f);
    p.weather = std::clamp(desc.weather, 0.0f, 1.0f);
    p.rain = std::clamp(desc.rain, 0.0f, 1.0f);
    p.leveling = desc.leveling;
    p.voice_file = desc.voice_file;
    p.voice_speaker = desc.voice_speaker;
    for (int i = 0; i < 3; ++i) p.forward[i] = desc.forward[i];
    p.entnum = desc.entnum;
    p.flags = (desc.is_voice ? kParamVoice : 0u) | (desc.dialogue ? kParamDialogue : 0u) |
              (desc.no_occlusion ? kParamNoOcclusion : 0u) | (desc.directional ? kParamDirectional : 0u) |
              (desc.outdoor_ambient ? kParamOutdoor : 0u) | (desc.free_ambient ? kParamFreeAmbient : 0u);
    v.params.store(p);
}

void SteamAudioEngine::update_channel(int ch_idx, const ChannelDescriptor& desc) {
    if (ch_idx < 0 || ch_idx >= kMaxChannels) return;
    const Slot& s = slots_[ch_idx];
    if (s.voice < 0 || s.entnum != desc.entnum || s.entchannel != desc.entchannel || s.mixer != desc.mixer) return;
    Voice& v = voices_[s.voice];
    if ((v.kind.load(std::memory_order_relaxed) == kMono3D) != desc.spatial_3d) return;
    publish_params(v, desc);
}

void SteamAudioEngine::update_brush_entities(const BrushEntityState* states, std::size_t count) {
    std::lock_guard<std::mutex> lk(brush_mutex_);
    if (count == 0 || !states) brush_states_.clear();
    else brush_states_.assign(states, states + count);
    brush_dirty_ = true;
}

void SteamAudioEngine::set_gain_model(const GainModel& model) { gain_model_.store(model); }

void SteamAudioEngine::set_engine_volume(float volume) {
    engine_volume_.store(sane_level(volume, engine_volume_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
}

void SteamAudioEngine::set_engine_ahead(int frames) {
    engine_ahead_.store(std::clamp(frames, 0, static_cast<int>(DryRing::kCapacity / 4 * 3)), std::memory_order_relaxed);
}

void SteamAudioEngine::begin_chunk(int chunk_frames) {
    chunk_frames_ = 0;
    if (!initialized_.load(std::memory_order_acquire) || chunk_frames <= 0) return;
    chunk_frames = std::min(chunk_frames, kMaxChunk);
    const uint32_t used = write_pos_.load(std::memory_order_relaxed) - read_pos_.load(std::memory_order_acquire);
    if (used + static_cast<uint32_t>(chunk_frames) + Resampler::kTaps > DryRing::kCapacity) {
        // The output device is not consuming (unplugged, being rerouted): drop rather than overwrite.
        dropped_chunks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    chunk_frames_ = chunk_frames;
}

int SteamAudioEngine::voice_for_feed(int ch_idx, const ChannelDescriptor& desc, VoiceKind kind) {
    Slot& s = slots_[ch_idx];
    if (s.voice >= 0) {
        if (voices_[s.voice].kind.load(std::memory_order_relaxed) == kind &&
            s.entnum == desc.entnum && s.entchannel == desc.entchannel && s.mixer == desc.mixer) {
            return s.voice;
        }
        retire_slot(ch_idx);
    }
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.state.load(std::memory_order_acquire) != kVoiceFree || v.p_fed_this_chunk) continue;
        if (!v.ring_l.ensure_allocated() || (kind == kStereo2D && !v.ring_r.ensure_allocated())) return -1;
        const uint32_t pos = write_pos_.load(std::memory_order_relaxed);
        v.kind.store(kind, std::memory_order_relaxed);
        v.start_pos.store(pos, std::memory_order_relaxed);
        v.written_end.store(pos, std::memory_order_relaxed);
        v.p_has_data = false;
        v.p_fade_done = kResumeFadeFrames;
        publish_params(v, desc);
        uint32_t gen = v.generation.load(std::memory_order_relaxed) + 1u;
        if (gen == 0u) gen = 1u;
        v.generation.store(gen, std::memory_order_release); // after its parameters: the raytracing thread reads it first
        v.state.store(kVoiceActive, std::memory_order_release);
        s.voice = i;
        s.entnum = desc.entnum;
        s.entchannel = desc.entchannel;
        s.mixer = desc.mixer;
        if (kind == kMono3D && !headless_) {
            // Trace its occlusion right away: the dry audio is heard snd_mixahead later, so the result is in time.
            {
                std::lock_guard<std::mutex> lk(sim_wake_mutex_);
                sim_kick_ = true;
            }
            sim_wake_.notify_one();
        }
        return i;
    }
    return -1;
}

void SteamAudioEngine::retire_slot(int ch_idx) {
    Slot& s = slots_[ch_idx];
    if (s.voice >= 0) voices_[s.voice].state.store(kVoiceDraining, std::memory_order_release);
    s = Slot{};
}

void SteamAudioEngine::feed_voice(int voice_idx, const float* left, const float* right, int offset, int count) {
    Voice& v = voices_[voice_idx];
    const bool stereo = (right != nullptr) && v.kind.load(std::memory_order_relaxed) == kStereo2D;
    const uint32_t n = static_cast<uint32_t>(count);
    const uint32_t pos = write_pos_.load(std::memory_order_relaxed) + static_cast<uint32_t>(offset);
    const uint32_t end = pos + n;
    uint32_t v_end = v.written_end.load(std::memory_order_relaxed);

    if (!v.p_has_data) {
        v.start_pos.store(pos, std::memory_order_release);
        v_end = pos;
        v.p_has_data = true;
    } else if (frame_diff(pos, v_end) > 0) {
        // The engine skipped this channel for a while (culled, paused): silence in between, then fade back in.
        const uint32_t gap = pos - v_end;
        v.ring_l.zero(v_end, gap);
        if (stereo) v.ring_r.zero(v_end, gap);
        v.p_fade_done = 0;
        v_end = pos;
    }

    v.ring_l.write(pos, left, n);
    if (stereo) v.ring_r.write(pos, right, n);
    if (v.p_fade_done < kResumeFadeFrames) {
        v.ring_l.apply_fade_in(pos, n, v.p_fade_done, kResumeFadeFrames);
        if (stereo) v.ring_r.apply_fade_in(pos, n, v.p_fade_done, kResumeFadeFrames);
        v.p_fade_done += n;
    }
    if (frame_diff(end, v_end) > 0) v_end = end;
    v.written_end.store(v_end, std::memory_order_release);

    if (!v.p_fed_this_chunk) {
        v.p_fed_this_chunk = true;
        fed_voices_[fed_count_++] = voice_idx;
    }
}

void SteamAudioEngine::feed_channel_mono(
    int ch_idx,
    const ChannelDescriptor& desc,
    const float* mono_samples,
    int offset_in_chunk,
    int count)
{
    if (ch_idx < 0 || ch_idx >= kMaxChannels || !mono_samples || count <= 0) return;
    if (offset_in_chunk < 0 || offset_in_chunk >= chunk_frames_) return;
    count = std::min(count, chunk_frames_ - offset_in_chunk);
    const int vi = voice_for_feed(ch_idx, desc, desc.spatial_3d ? kMono3D : kMono2D);
    if (vi < 0) return;
    publish_params(voices_[vi], desc);
    feed_voice(vi, mono_samples, nullptr, offset_in_chunk, count);
}

void SteamAudioEngine::feed_channel_stereo(
    int ch_idx,
    const ChannelDescriptor& desc,
    const float* left_samples,
    const float* right_samples,
    int offset_in_chunk,
    int count)
{
    if (!left_samples || !right_samples) return;
    if (desc.spatial_3d) {
        // Fold stereo 3D source to mono for binaural HRTF processing.
        float mono[kMaxChunk];
        count = std::min(count, kMaxChunk);
        for (int i = 0; i < count; ++i) mono[i] = 0.5f * (left_samples[i] + right_samples[i]);
        feed_channel_mono(ch_idx, desc, mono, offset_in_chunk, count);
        return;
    }
    if (ch_idx < 0 || ch_idx >= kMaxChannels || count <= 0) return;
    if (offset_in_chunk < 0 || offset_in_chunk >= chunk_frames_) return;
    count = std::min(count, chunk_frames_ - offset_in_chunk);
    const int vi = voice_for_feed(ch_idx, desc, kStereo2D);
    if (vi < 0) return;
    publish_params(voices_[vi], desc);
    feed_voice(vi, left_samples, right_samples, offset_in_chunk, count);
}

// Takes back what voices wrote past `limit` in the chunk that is being finished. Never touches committed frames.
void SteamAudioEngine::trim_fed_voices(uint32_t limit) {
    for (int k = 0; k < fed_count_; ++k) {
        Voice& v = voices_[fed_voices_[k]];
        if (frame_diff(v.start_pos.load(std::memory_order_relaxed), limit) >= 0) {
            v.start_pos.store(limit, std::memory_order_release);
            v.p_has_data = false;
        }
        if (frame_diff(v.written_end.load(std::memory_order_relaxed), limit) > 0) {
            v.written_end.store(limit, std::memory_order_release);
        }
        v.p_fed_this_chunk = false;
    }
    fed_count_ = 0;
}

void SteamAudioEngine::end_chunk(int actual_chunk_frames) {
    int n = chunk_frames_;
    chunk_frames_ = 0;
    if (actual_chunk_frames > 0) n = std::min(n, actual_chunk_frames);
    if (n <= 0) {
        trim_fed_voices(write_pos_.load(std::memory_order_relaxed));
        return;
    }

    uint32_t c3d = 0, c2d = 0;
    for (int k = 0; k < fed_count_; ++k) {
        if (voices_[fed_voices_[k]].kind.load(std::memory_order_relaxed) == kMono3D) ++c3d;
        else ++c2d;
    }
    active_3d_count_.store(c3d, std::memory_order_relaxed);
    active_2d_count_.store(c2d, std::memory_order_relaxed);

    const uint32_t end = write_pos_.load(std::memory_order_relaxed) + static_cast<uint32_t>(n);
    trim_fed_voices(end);
    write_pos_.store(end, std::memory_order_release);
}

void SteamAudioEngine::discard_chunk() {
    chunk_frames_ = 0;
    trim_fed_voices(write_pos_.load(std::memory_order_relaxed));
}

void SteamAudioEngine::notify_channel_freed(int ch_idx) {
    if (ch_idx < 0 || ch_idx >= kMaxChannels) return;
    retire_slot(ch_idx);
}

bool SteamAudioEngine::channel_has_voice(int ch_idx) const {
    return ch_idx >= 0 && ch_idx < kMaxChannels && slots_[ch_idx].voice >= 0;
}

void SteamAudioEngine::flush_audio() {
    discard_chunk();
    for (int i = 0; i < kMaxChannels; ++i) retire_slot(i);
    flush_to_.store(write_pos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    flush_epoch_.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------------------------------------------

void SteamAudioEngine::pace_output(ma_device* dev) {
    // miniaudio renders a period as soon as it can and only then waits until WASAPI has room for it, so a whole buffer
    // of finished audio (about 22 ms) always lies between the camera and the ears. Wait first: for the moment when the
    // audio engine took all but what keeps it fed until the next wake-up.
    auto* client = reinterpret_cast<ma_IAudioClient*>(dev->wasapi.pAudioClientPlayback);
    const auto event = reinterpret_cast<HANDLE>(dev->wasapi.hEventPlayback);
    if (!client || !event || dev->playback.shareMode != ma_share_mode_shared || dev->wasapi.pMappedBufferPlayback) return;
    const uint32_t buffer = dev->wasapi.actualBufferSizeInFramesPlayback;
    const uint32_t period = dev->wasapi.periodSizeInFramesPlayback;
    const uint32_t rate = dev->playback.internalSampleRate;
    if (buffer == 0 || period == 0 || rate < 1000) return;
    if (pacer_.configure(buffer, period, rate)) reserve_.store(pacer_.reserve(), std::memory_order_relaxed);

    UINT32 padding = 0;
    for (int i = 0; i < 64; ++i) {
        if (ma_device_get_state(dev) != ma_device_state_started) return;
        if (FAILED(ma_IAudioClient_GetCurrentPadding(client, &padding))) return; // unplugged: the device thread handles it
        if (padding <= pacer_.reserve()) break;
        if (WaitForSingleObject(event, 50) != WAIT_OBJECT_0) break;
    }

    if (pacer_.woke(now_us(), padding)) {
        late_wakes_.fetch_add(1, std::memory_order_relaxed);
        reserve_.store(pacer_.reserve(), std::memory_order_relaxed);
    }
    queued_avg_.store(static_cast<uint32_t>(pacer_.queued_average() + 0.5f), std::memory_order_relaxed);
    if (padding > queued_max_.load(std::memory_order_relaxed)) queued_max_.store(padding, std::memory_order_relaxed);
}

void SteamAudioEngine::render_output(float* out_interleaved_lr, uint32_t frames) {
    if (!out_interleaved_lr) return;
    while (frames > 0) {
        if (carry_pos_ == carry_len_) {
            render_block();
            carry_pos_ = 0;
            carry_len_ = static_cast<uint32_t>(frame_);
        }
        const uint32_t n = std::min(frames, carry_len_ - carry_pos_);
        std::memcpy(out_interleaved_lr, carry_.data() + carry_pos_ * 2, n * 2 * sizeof(float));
        out_interleaved_lr += n * 2;
        carry_pos_ += n;
        frames -= n;
    }
}

void SteamAudioEngine::set_env_fade_out(int frames) {
    for (int i = 0; i < frame_; ++i) {
        env_[i] = i < frames ? 1.0f - static_cast<float>(i + 1) / static_cast<float>(frames) : 0.0f;
    }
}

void SteamAudioEngine::set_env_fade_in() {
    for (int i = 0; i < frame_; ++i) env_[i] = static_cast<float>(i + 1) / static_cast<float>(frame_);
}

// Decides which dry frames the next block plays. Fills env_ when the block has to fade.
SteamAudioEngine::BlockPlan SteamAudioEngine::plan_block() {
    BlockPlan plan{};
    plan.write = write_pos_.load(std::memory_order_acquire);
    plan.read = read_pos_.load(std::memory_order_relaxed);
    plan.frac = a_frac_;

    const double base_step = static_cast<double>(kDryRate) / rate_;
    const auto set_step = [&](double step) {
        plan.step = step;
        const double end = plan.frac + step * frame_;
        plan.advance = static_cast<uint32_t>(end + 1e-7);
        plan.next_frac = end - static_cast<double>(plan.advance);
        if (plan.next_frac < 1e-6) plan.next_frac = 0.0;
        plan.direct = step == 1.0 && plan.frac == 0.0;
        plan.frames = plan.direct ? static_cast<uint32_t>(frame_)
                                  : static_cast<uint32_t>(plan.frac + step * (frame_ - 1)) + 1u + Resampler::kAhead;
    };

    const uint32_t epoch = flush_epoch_.load(std::memory_order_acquire);
    if (epoch != a_flush_epoch_) {
        a_flush_epoch_ = epoch;
        const uint32_t left = static_cast<uint32_t>(std::max(0, frame_diff(flush_to_.load(std::memory_order_relaxed), plan.read)));
        set_step(base_step);
        plan.frames = a_starved_ ? 0u : std::min(left, plan.frames);
        plan.advance = left;
        plan.next_frac = 0.0;
        plan.flush = true;
        set_env_fade_out(frame_);
        a_starved_ = true;
        return plan;
    }

    const bool held = held_.load(std::memory_order_relaxed);
    if (held) {
        if (a_held_ || a_starved_) { // the engine's clock stands still, and so does the timeline
            a_held_ = true;
            return plan;
        }
        a_held_ = true; // this block fades out
    } else if (a_held_) {
        a_held_ = false;
        if (!a_starved_) a_fade_in_ = true;
        a_surplus_blocks_ = 0;
    }

    int32_t fill = frame_diff(plan.write, plan.read);
    const int32_t ahead = engine_ahead_.load(std::memory_order_relaxed);

    if (a_starved_) {
        const int32_t need = std::max(min_fill_, ahead - kSurplusHigh);
        if (fill < need || !enabled_.load(std::memory_order_relaxed)) return plan; // nothing to play yet
        const int32_t target = std::max(min_fill_, ahead);
        if (ahead > 0 && fill > target + kSurplusHigh) {
            // The stall left more behind than the engine's playback clock would still play: rejoin the clock.
            plan.read += static_cast<uint32_t>(fill - target);
            fill = target;
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }
        plan.frac = 0.0;
        a_starved_ = false;
        a_fade_in_ = true;
        a_skip_frames_ = 0;
        a_rate_step_ = 0;
        a_surplus_blocks_ = 0;
    }

    if (fill < min_fill_) {
        set_step(base_step);
        const uint32_t left = static_cast<uint32_t>(std::max(fill, 0));
        plan.frames = std::min(left, plan.frames);
        plan.advance = std::min(left, plan.advance);
        plan.next_frac = 0.0;
        plan.use_env = true;
        set_env_fade_out(std::min(frame_, static_cast<int>(static_cast<double>(left) / base_step)));
        a_starved_ = true;
        starvations_.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }

    // Right after the engine paints, an aligned timeline holds exactly `ahead` frames; between paints it holds less.
    // So the largest surplus of a window shows how far playback trails (or leads) the engine's clock.
    if (ahead > 0) {
        const int32_t surplus = fill - ahead;
        if (a_surplus_blocks_ == 0 || surplus > a_max_surplus_) a_max_surplus_ = surplus;
        if (++a_surplus_blocks_ >= surplus_window_) {
            a_surplus_blocks_ = 0;
            if (a_max_surplus_ > kSurplusSkip) {
                a_skip_frames_ = static_cast<uint32_t>(a_max_surplus_);
                a_rate_step_ = 0;
            } else if (a_max_surplus_ > kSurplusHigh || (a_rate_step_ > 0 && a_max_surplus_ > kSurplusHighDone)) {
                a_rate_step_ = 1;
            } else if (a_max_surplus_ < kSurplusLow || (a_rate_step_ < 0 && a_max_surplus_ < kSurplusLowDone)) {
                a_rate_step_ = -1;
            } else {
                a_rate_step_ = 0;
            }
        }
    }

    if (rate_ == kDryRate) {
        // Half a frame more or less per block (0.4 %); it ends on a whole frame, where nothing needs interpolating.
        const int half = a_rate_step_ != 0 ? a_rate_step_ : (plan.frac != 0.0 ? 1 : 0);
        set_step(half != 0 ? (frame_ + 0.5 * half) / frame_ : 1.0);
    } else {
        set_step(base_step * (1.0 + 0.004 * a_rate_step_));
    }

    if (held) {
        a_fade_in_ = false;
        a_skip_frames_ = 0;
        plan.use_env = true;
        set_env_fade_out(frame_);
        return plan;
    }
    if (a_skip_frames_ > 0) {
        const int32_t room = fill - static_cast<int32_t>(plan.advance) - 2 * min_fill_;
        if (room > 0) plan.advance += std::min(a_skip_frames_, static_cast<uint32_t>(room));
        a_skip_frames_ = 0;
        a_fade_in_ = true; // the next block
        plan.use_env = true;
        set_env_fade_out(frame_);
        resyncs_.fetch_add(1, std::memory_order_relaxed);
    } else if (a_fade_in_) {
        a_fade_in_ = false;
        plan.use_env = true;
        set_env_fade_in();
    }
    return plan;
}

// Reads one block of a voice's dry PCM into dst[frame_], at the output rate; frames the voice does not cover are silent.
void SteamAudioEngine::fetch_dry(const Voice& v, const DryRing& ring, const BlockPlan& plan, float* dst) {
    if (plan.frames == 0) {
        std::fill(dst, dst + frame_, 0.0f);
        return;
    }
    const uint32_t history = plan.direct ? 0u : static_cast<uint32_t>(Resampler::kHistory);
    const uint32_t base = plan.read - history;
    const int n = static_cast<int>(plan.frames + history);
    float* f = fetch_.data();
    std::memset(f, 0, fetch_.size() * sizeof(float));

    const uint32_t end = v.written_end.load(std::memory_order_acquire);
    const uint32_t start = v.start_pos.load(std::memory_order_acquire);
    const int end_off = frame_diff(end, base);
    const int a = std::clamp(static_cast<int>(frame_diff(start, base)), 0, n);
    const int b = std::clamp(end_off, 0, n);
    if (b > a) ring.read(base + static_cast<uint32_t>(a), f + a, static_cast<uint32_t>(b - a));

    // The timeline goes on without this voice: it stops here, possibly mid-waveform.
    if (frame_diff(plan.write, end) > 0) {
        for (int k = std::max(a, end_off - static_cast<int>(kEndFadeFrames)); k < b; ++k) {
            f[k] *= static_cast<float>(end_off - k) / static_cast<float>(kEndFadeFrames);
        }
    }

    if (plan.direct) std::memcpy(dst, f, static_cast<std::size_t>(frame_) * sizeof(float));
    else resampler_.process(f, dst);
    if (plan.use_env) {
        for (int i = 0; i < frame_; ++i) dst[i] *= env_[i];
    }
}

bool SteamAudioEngine::send_to_reverb(const float* mono, float gain0, float gain1) {
    if (std::max(gain0, gain1) < 1e-5f) return false;
    if (!reverb_on_.load(std::memory_order_relaxed) || !refl_valid_.load(std::memory_order_relaxed)) return false;
    const float step = (gain1 - gain0) / static_cast<float>(frame_);
    for (int s = 0; s < frame_; ++s) rev_send_[s] += mono[s] * (gain0 + step * static_cast<float>(s + 1));
    return true;
}

// Rain through a roof: quieter and duller. Two one-pole stages (12 dB per octave), in place.
void SteamAudioEngine::apply_weather(Voice& v, int side, float* samples, float gain0, float gain1, float coeff0, float coeff1) {
    float* z = v.a_lp[side];
    const float inv = 1.0f / static_cast<float>(frame_);
    for (int s = 0; s < frame_; ++s) {
        const float t = static_cast<float>(s + 1) * inv;
        const float c = coeff0 + (coeff1 - coeff0) * t;
        z[0] += (samples[s] - z[0]) * c;
        z[1] += (z[0] - z[1]) * c;
        samples[s] = z[1] * (gain0 + (gain1 - gain0) * t);
    }
    if (std::fabs(z[0]) < 1e-12f) z[0] = 0.0f;
    if (std::fabs(z[1]) < 1e-12f) z[1] = 0.0f;
}

// Voices: hears what engine.dll has mixed but is not played yet (snd_mixahead), so the gain is right when it is.
void SteamAudioEngine::listen_ahead(Voice& v, const BlockPlan& plan, uint32_t kind) {
    const Leveling leveling = v.a_params.leveling;
    if (leveling == Leveling::kNone || leveling == Leveling::kStinger) return;
    const uint32_t played = plan.read + plan.advance; // to the end of this block
    const uint32_t start = v.start_pos.load(std::memory_order_acquire);
    uint32_t end = v.written_end.load(std::memory_order_acquire);
    if (frame_diff(end, plan.write) > 0) end = plan.write; // the rest may still be taken back
    const uint32_t ahead = played + static_cast<uint32_t>(VoiceLeveler::kLookAheadSeconds * static_cast<float>(kDryRate));
    if (frame_diff(end, ahead) > 0) end = ahead;

    uint32_t from = v.a_listened;
    if (v.a_voice_new) {
        v.a_voice_new = false;
        float known_db = 0.0f, known_seconds = 0.0f;
        a_voice_memory_.recall(v.a_params.voice_file, v.a_params.voice_speaker, known_db, known_seconds);
        v.a_voice.reset(known_db, known_seconds);
        from = start;
    }
    if (frame_diff(plan.read, from) > 0) from = plan.read;                       // skipped (resync), or joined late
    if (frame_diff(from, ahead) > 0) from = frame_diff(plan.read, start) > 0 ? plan.read : start; // the clock jumped back
    if (frame_diff(start, from) > 0) from = start;
    while (frame_diff(end, from) > 0) {
        const uint32_t n = std::min(end - from, static_cast<uint32_t>(listen_l_.size()));
        v.ring_l.read(from, listen_l_.data(), n);
        const float* right = nullptr;
        if (kind == kStereo2D) {
            v.ring_r.read(from, listen_r_.data(), n);
            right = listen_r_.data();
        }
        v.a_voice.listen(listen_l_.data(), right, static_cast<int>(n), static_cast<float>(kDryRate));
        from += n;
    }
    v.a_listened = from;
    v.a_voice_ahead = static_cast<float>(std::max<int32_t>(0, frame_diff(from, played))) / static_cast<float>(kDryRate);
}

// A line has been played (or was cut off): next time it is known.
void SteamAudioEngine::remember_voice(Voice& v) {
    if (v.a_voice_new) return;
    v.a_voice_new = true;
    if (v.a_params.leveling == Leveling::kBark || v.a_params.leveling == Leveling::kDialogue) {
        a_voice_memory_.remember(v.a_params.voice_file, v.a_params.voice_speaker, v.a_voice.db(), v.a_voice.seconds());
    }
}

// Voices are brought to one loudness, hot effects are turned down; in place. `scale`: how loud an effect is played.
void SteamAudioEngine::level_voice(Voice& v, float* left, float* right, float scale, bool first) {
    const Leveling leveling = v.a_params.leveling;
    if (first) v.a_leveler.reset();
    float gain_db = 0.0f;
    if (leveling == Leveling::kStinger) {
        v.a_leveler.process(left, right, frame_, static_cast<float>(rate_), kStingerLeveler, scale);
        gain_db = v.a_leveler.cut_db();
    } else if (!v.a_voice_new) { // also back to the recording, if the tweak was turned off meanwhile
        const VoiceLevelSettings& settings = leveling == Leveling::kDialogue ? kDialogueVoice
                                             : (leveling == Leveling::kCry ? kCryVoice : kBarkVoice);
        v.a_voice.process(left, right, frame_, static_cast<float>(rate_), settings, v.a_voice_ahead, leveling != Leveling::kNone);
        gain_db = v.a_voice.gain_db();
    }
    v.dbg_level.store(gain_db, std::memory_order_relaxed);
    v.dbg_recorded.store(!v.a_voice_new && v.a_voice.seconds() > 0.0f ? v.a_voice.db() : -120.0f, std::memory_order_relaxed);
}

// After all voices of a block: how much the rain they contained covers the next one.
void SteamAudioEngine::update_rain() {
    const float heard = a_rain_in_;
    a_rain_in_ = 0.0f;
    a_raining_ = heard > (a_raining_ ? kRainOff : kRainOn);
    const float target = a_raining_ ? heard : 0.0f;
    a_rain_ += (target - a_rain_) * (target > a_rain_ ? rain_up_alpha_ : rain_down_alpha_);
    if (a_rain_ < 1e-4f) a_rain_ = 0.0f;
    a_cover_ = a_rain_ * rain_masking_.load(std::memory_order_relaxed);
    rain_level_.store(a_rain_, std::memory_order_relaxed);
}

void SteamAudioEngine::render_voice(Voice& v, const BlockPlan& plan, bool& any_reverb_input) {
    const uint32_t state = v.state.load(std::memory_order_acquire);
    if (state == kVoiceFree) return;

    const uint32_t generation = v.generation.load(std::memory_order_relaxed);
    if (generation != v.a_generation) {
        v.a_generation = generation;
        v.a_smooth_init = false;
        v.a_tail_blocks = 0;
        v.a_voice_new = true;
    }

    const uint32_t end = v.written_end.load(std::memory_order_acquire);
    const uint32_t start = v.start_pos.load(std::memory_order_acquire);
    const int32_t history = (plan.direct || plan.frames == 0) ? 0 : Resampler::kHistory;
    const int32_t from = std::max(-history, frame_diff(start, plan.read));
    const int32_t to = std::min(static_cast<int32_t>(plan.frames), frame_diff(end, plan.read));
    const bool has_data = to > from;
    if (has_data) {
        v.a_tail_blocks = tail_blocks_;
    } else if (v.a_tail_blocks > 0) {
        --v.a_tail_blocks;
    } else {
        v.a_smooth_init = false; // the next data starts from the then-current position and volume
        if (state == kVoiceDraining && frame_diff(end, plan.read + plan.advance) <= 0) {
            remember_voice(v);
            v.state.store(kVoiceFree, std::memory_order_release);
        }
        return;
    }

    v.params.load(v.a_params);
    const float target_master = v.a_params.master_gain;
    const uint32_t kind = v.kind.load(std::memory_order_relaxed);
    const bool first = !v.a_smooth_init;
    v.a_smooth_init = true;

    const float master0 = first ? target_master : v.a_master;
    const float master1 = master0 + (target_master - master0) * gain_alpha_;
    v.a_master = master1;
    if (has_data) listen_ahead(v, plan, kind);
    const float master_step = (master1 - master0) / static_cast<float>(frame_);

    if (kind != kMono3D) {
        // 2D / UI / music: clean dry mix.
        if (!has_data) return;
        fetch_dry(v, v.ring_l, plan, scratch_dry_l_.data());
        float* left = scratch_dry_l_.data();
        float* right = left;
        if (kind == kStereo2D) {
            fetch_dry(v, v.ring_r, plan, scratch_dry_r_.data());
            right = scratch_dry_r_.data();
        }
        const float weather = v.a_params.weather;
        float weather_gain = 1.0f;
        if (weather > 0.0f) {
            const float exposure = weather_occlusion_on_.load(std::memory_order_relaxed)
                                       ? sky_exposure_.load(std::memory_order_relaxed) : 1.0f;
            const float open_target = 1.0f - weather * (1.0f - weather_openness(exposure));
            if (first) {
                v.a_weather = open_target;
                std::memset(v.a_lp, 0, sizeof(v.a_lp));
            }
            const float open0 = v.a_weather;
            const float open1 = open0 + (open_target - open0) * weather_alpha_;
            v.a_weather = open1;
            const float trim = 1.0f + weather * (weather_volume_.load(std::memory_order_relaxed) - 1.0f);
            const float g0 = trim * (kCoveredGain + (1.0f - kCoveredGain) * open0);
            const float g1 = trim * (kCoveredGain + (1.0f - kCoveredGain) * open1);
            const float rate = static_cast<float>(rate_);
            const float c0 = weather_cutoff_coeff(open0, rate), c1 = weather_cutoff_coeff(open1, rate);
            apply_weather(v, 0, left, g0, g1, c0, c1);
            if (right != left) apply_weather(v, 1, right, g0, g1, c0, c1);
            weather_gain = g1;
            // It covers other sounds as much as it is heard itself.
            a_rain_in_ = std::max(a_rain_in_, v.a_params.rain * open1);
        } else if ((v.a_params.flags & kParamOutdoor) != 0u) {
            // The city's background in the rain.
            const float cover1 = std::min(1.0f, a_cover_);
            const float cover0 = first ? cover1 : v.a_cover;
            v.a_cover = cover1;
            if (first) std::memset(v.a_lp, 0, sizeof(v.a_lp));
            const float rate = static_cast<float>(rate_);
            const auto coeff = [&](float cover) {
                return cover < 1e-3f ? 1.0f : lowpass_coeff(kOpenCutoffHz * std::pow(kRainAmbientHz / kOpenCutoffHz, cover), rate);
            };
            const float g0 = 1.0f - kRainAmbient * cover0, g1 = 1.0f - kRainAmbient * cover1;
            apply_weather(v, 0, left, g0, g1, coeff(cover0), coeff(cover1));
            if (right != left) apply_weather(v, 1, right, g0, g1, coeff(cover0), coeff(cover1));
            weather_gain = g1;
        }
        level_voice(v, left, right, std::max(master0, master1), first);
        const float send1 = v.a_params.reverb_gain * kNearSend; // the player's own steps and shots; not UI and music
        const float send0 = first ? send1 : v.a_reverb;
        v.a_reverb = send1;
        for (int s = 0; s < frame_; ++s) {
            const float g = kLevel2D * (master0 + master_step * static_cast<float>(s + 1));
            mix_l_[s] += left[s] * g;
            mix_r_[s] += right[s] * g;
        }
        if (send_to_reverb(left, master0 * send0, master1 * send1)) any_reverb_input = true;
        v.dbg_distance.store(0.0f, std::memory_order_relaxed);
        v.dbg_atten.store(1.0f, std::memory_order_relaxed);
        v.dbg_occ.store(1.0f, std::memory_order_relaxed);
        v.dbg_send.store(send1, std::memory_order_relaxed);
        v.dbg_weather_gain.store(weather_gain, std::memory_order_relaxed);
        return;
    }

    // 3D: everything below uses the newest published listener pose and source position.
    v.a_pos = source_pos_to_ipl(v.a_params.origin);
    float dist_m = 0.0f;
    IPLVector3 local_dir = world_dir_to_listener_local(a_listener_, v.a_pos, &dist_m);
    SimPlace place{};
    const bool placed = v.place.load(place) && place.generation == generation;
    if (placed && place.relocated && (v.a_params.flags & kParamFreeAmbient) != 0u) {
        // As loud as the game wants it (distance), but from where the street is.
        local_dir = world_dir_to_listener_local(a_listener_, place.position, nullptr);
    }
    float target_atten = compute_source_distance_attenuation(dist_m, v.a_params.dist_mult, a_gain_model_);
    if (v.a_params.loop_master > 0.0f) target_atten *= loop_cull_window(v.a_params.loop_master * target_atten);

    const uint32_t flags = v.a_params.flags;
    const bool is_voice = (flags & kParamVoice) != 0u;
    float target_occ = 1.0f;
    float target_trans[3] = { 1.0f, 1.0f, 1.0f };
    SimDirect sim{};
    if ((flags & kParamNoOcclusion) == 0u && occlusion_on_.load(std::memory_order_relaxed) &&
        v.sim.load(sim) && sim.generation == generation) {
        target_occ = sim.occlusion;
        // The engine never takes more than ~11 dB off a voice behind a wall; dialogue has to stay understandable.
        const float* floor_trans = is_voice ? kVoiceTransmission : kMinTransmission;
        for (int b = 0; b < 3; ++b) target_trans[b] = std::max(sim.transmission[b], floor_trans[b]);
    }

    VoiceDirectivity target_dir{};
    if ((flags & kParamDirectional) != 0u && directivity_on_.load(std::memory_order_relaxed)) {
        target_dir = voice_directivity(source_dir_to_ipl(v.a_params.forward), v.a_pos, a_listener_.origin);
    }

    // Conversations through glass. The first line starts as the one before it ended, until its rays are back.
    float target_glass = 0.0f;
    if ((flags & kParamDialogue) != 0u && glass_on_.load(std::memory_order_relaxed)) {
        if (placed) target_glass = place.glass;
        else if (v.a_params.entnum == a_glass_entity_) target_glass = a_glass_;
        a_glass_entity_ = v.a_params.entnum;
        a_glass_ = target_glass;
    }
    v.a_glass = first ? target_glass : v.a_glass + (target_glass - v.a_glass) * glass_alpha_;

    // Rain between the listener and something far away; not what is said to the player.
    float rain_level = 1.0f, rain_mid = 1.0f, rain_high = 1.0f;
    if (a_cover_ > 0.0f && (flags & kParamDialogue) == 0u) {
        if ((flags & kParamOutdoor) != 0u) {
            const float t = std::min(1.0f, a_cover_);
            rain_level = 1.0f - kRainAmbient * t;
            rain_mid = 1.0f - kRainAmbientMid * t;
            rain_high = 1.0f - kRainAmbientHigh * t;
        } else {
            const float t = std::min(1.25f, a_cover_ * rain_distance(dist_m));
            rain_level = 1.0f - kRainLevel * t;
            rain_mid = 1.0f - kRainMid * t;
            rain_high = 1.0f - kRainHigh * t;
        }
    }
    target_atten *= rain_level;

    if (first) {
        v.a_dir = target_dir;
        v.a_atten = target_atten;
        v.a_occ = target_occ;
        for (int b = 0; b < 3; ++b) v.a_trans[b] = target_trans[b];
        v.a_reset_fx = true;
    } else {
        v.a_dir.gain += (target_dir.gain - v.a_dir.gain) * dir_alpha_;
        v.a_dir.mid += (target_dir.mid - v.a_dir.mid) * dir_alpha_;
        v.a_dir.high += (target_dir.high - v.a_dir.high) * dir_alpha_;
        v.a_atten += (target_atten - v.a_atten) * gain_alpha_;
        v.a_occ += (target_occ - v.a_occ) * occ_alpha_;
        for (int b = 0; b < 3; ++b) v.a_trans[b] += (target_trans[b] - v.a_trans[b]) * occ_alpha_;
    }

    const float send1 = v.a_params.reverb_gain * distance_send(dist_m, is_voice) *
                        ((flags & kParamDialogue) != 0u ? kDialogueSend * (1.0f - (1.0f - kGlassSend) * v.a_glass) : 1.0f) *
                        v.a_atten *
                        (kOccludedSend + (1.0f - kOccludedSend) * v.a_occ);
    const float send0 = first ? send1 : v.a_reverb;
    v.a_reverb = send1;
    v.dbg_distance.store(dist_m, std::memory_order_relaxed);
    v.dbg_atten.store(v.a_atten, std::memory_order_relaxed);
    v.dbg_occ.store(v.a_occ, std::memory_order_relaxed);
    v.dbg_send.store(send1, std::memory_order_relaxed);
    v.dbg_weather_gain.store(rain_level, std::memory_order_relaxed);
    v.dbg_glass.store(v.a_glass, std::memory_order_relaxed);
    v.dbg_directivity.store(v.a_dir.gain, std::memory_order_relaxed);

    if (v.a_atten * std::max(master0, master1) < kSilentGain && target_atten * target_master < kSilentGain) {
        v.a_reset_fx = true; // inaudible: skip the DSP, start clean when it comes back
        return;
    }
    const bool fresh_fx = v.a_reset_fx;
    if (fresh_fx) {
        iplDirectEffectReset(v.direct_effect);
        iplBinauralEffectReset(v.binaural_effect);
        v.a_reset_fx = false;
    }

    if (has_data) {
        fetch_dry(v, v.ring_l, plan, scratch_mono_.data());
        for (int s = 0; s < frame_; ++s) {
            scratch_mono_[s] *= master0 + master_step * static_cast<float>(s + 1);
        }
        level_voice(v, scratch_mono_.data(), nullptr, 1.0f, first);
    } else {
        std::fill(scratch_mono_.begin(), scratch_mono_.end(), 0.0f); // let the HRTF filter ring out
    }

    IPLDirectEffectParams dir_params = make_default_direct_params(dist_m, v.a_atten);
    dir_params.flags = static_cast<IPLDirectEffectFlags>(
        dir_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION |
        IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
    // The direct path only: the room hears all of a voice, wherever the character looks.
    const auto through_glass = [&](int band) { return 1.0f + (kGlassBands[band] - 1.0f) * v.a_glass; };
    dir_params.directivity = v.a_dir.gain * through_glass(0);
    dir_params.airAbsorption[1] *= v.a_dir.mid * rain_mid * through_glass(1) / through_glass(0);
    dir_params.airAbsorption[2] *= v.a_dir.high * rain_high * through_glass(2) / through_glass(0);
    dir_params.occlusion = v.a_occ;
    for (int b = 0; b < 3; ++b) dir_params.transmission[b] = v.a_trans[b];

    IPLfloat32* in_ptrs[1]  = { scratch_mono_.data() };
    IPLfloat32* dir_ptrs[1] = { scratch_direct_.data() };
    IPLAudioBuffer in_buf{ 1, frame_, in_ptrs };
    IPLAudioBuffer dir_buf{ 1, frame_, dir_ptrs };
    iplDirectEffectApply(v.direct_effect, &dir_params, &in_buf, &dir_buf);

    // Feed the shared reflection/reverb bus.
    if (has_data && send_to_reverb(scratch_mono_.data(), send0, send1)) any_reverb_input = true;

    const float blend = compute_spatial_blend(dist_m, v.a_params.radius);
    if (hrtf_on_.load(std::memory_order_relaxed)) {
        IPLfloat32* bin_ptrs[2] = { scratch_bin_l_.data(), scratch_bin_r_.data() };
        IPLAudioBuffer bin_buf{ 2, frame_, bin_ptrs };
        IPLBinauralEffectParams bparams{};
        bparams.direction = local_dir;
        bparams.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        bparams.spatialBlend = blend;
        bparams.hrtf = ipl_hrtf_;
        iplBinauralEffectApply(v.binaural_effect, &bparams, &dir_buf, &bin_buf); // cross-fades from the last direction
        for (int s = 0; s < frame_; ++s) {
            mix_l_[s] += scratch_bin_l_[s];
            mix_r_[s] += scratch_bin_r_[s];
        }
    } else {
        // Equal-power stereo pan fallback when HRTF is toggled off.
        const float pan = std::clamp(local_dir.x, -1.0f, 1.0f) * blend;
        const float angle = (pan + 1.0f) * 0.78539816f; // 0..pi/2
        const float l1 = std::cos(angle), r1 = std::sin(angle);
        const float l0 = fresh_fx ? l1 : v.a_pan_l, r0 = fresh_fx ? r1 : v.a_pan_r;
        for (int s = 0; s < frame_; ++s) {
            const float t = static_cast<float>(s + 1) / static_cast<float>(frame_);
            mix_l_[s] += scratch_direct_[s] * (l0 + (l1 - l0) * t);
            mix_r_[s] += scratch_direct_[s] * (r0 + (r1 - r0) * t);
        }
        v.a_pan_l = l1;
        v.a_pan_r = r1;
    }
}

void SteamAudioEngine::render_block() {
    const uint32_t t0 = now_us();

    ListenerPose pose{};
    if (listener_pub_.load(pose)) {
        a_listener_ = pose.cs;
        pose_age_us_.store(t0 - pose.stamp_us, std::memory_order_relaxed);
    }
    gain_model_.load(a_gain_model_);

    std::fill(mix_l_.begin(), mix_l_.end(), 0.0f);
    std::fill(mix_r_.begin(), mix_r_.end(), 0.0f);
    std::fill(rev_send_.begin(), rev_send_.end(), 0.0f);

    const BlockPlan plan = plan_block();
    if (!plan.direct && plan.frames > 0) resampler_.prepare(plan.frac, plan.step, frame_);
    bool any_reverb_input = false;
    for (Voice& v : voices_) render_voice(v, plan, any_reverb_input);
    update_rain();
    a_frac_ = plan.next_frac;
    read_pos_.store(plan.read + plan.advance, std::memory_order_release);

    // Everything fades in the background and before the device is swapped, the reverb while the game is paused. Once the
    // reverb is silent its effects forget what they held, so nothing old is heard when it comes back.
    const auto towards = [](float from, float to, float down, float up) {
        return from < to ? std::min(to, from + up) : std::max(to, from - down);
    };
    const bool muted = swap_mute_.load(std::memory_order_relaxed) ||
                       (!focused_.load(std::memory_order_relaxed) && mute_in_background_.load(std::memory_order_relaxed));
    const float gate0 = a_gate_, gate1 = towards(gate0, muted ? 0.0f : 1.0f, gate_step_, gate_step_);
    a_gate_ = gate1;
    const bool silent = gate0 <= 0.0f && gate1 <= 0.0f;
    const float room0 = a_room_;
    const float room1 = towards(room0, game_paused_.load(std::memory_order_relaxed) ? 0.0f : 1.0f, room_down_step_, room_up_step_);
    a_room_ = room1;
    if (silent || (room0 <= 0.0f && room1 <= 0.0f)) {
        if (a_room_live_) {
            iplReflectionEffectReset(refl_effect_);
            iplAmbisonicsDecodeEffectReset(ambi_decode_);
            a_room_live_ = false;
        }
        a_reverb_tail_blocks_ = 0;
        any_reverb_input = false;
    }

    // Real-time BSP-raytraced reverb, decoded binaurally with the same listener pose.
    const float wet = reverb_wet_.load(std::memory_order_relaxed) *
                      (reverb_mode_ == ReverbMode::kHybrid ? kHybridGain : kParametricGain);
    if (any_reverb_input) a_reverb_tail_blocks_ = reverb_tail_;
    if (reverb_on_.load(std::memory_order_relaxed) && refl_valid_.load(std::memory_order_acquire) &&
        wet > 1e-4f && a_reverb_tail_blocks_ > 0) {
        if (refl_pub_.load(a_refl_)) a_refl_known_ = true;
        --a_reverb_tail_blocks_;
        if (a_refl_known_) {
            a_room_live_ = true;
            IPLfloat32* rev_in_ptrs[1] = { rev_send_.data() };
            IPLfloat32* ambi_ptrs[kAmbisonicsChannels] = {
                rev_ambi_[0].data(), rev_ambi_[1].data(), rev_ambi_[2].data(), rev_ambi_[3].data()
            };
            IPLAudioBuffer rev_in_buf{ 1, frame_, rev_in_ptrs };
            IPLAudioBuffer ambi_buf{ kAmbisonicsChannels, frame_, ambi_ptrs };
            iplReflectionEffectApply(refl_effect_, &a_refl_, &rev_in_buf, &ambi_buf, nullptr);

            IPLfloat32* rev_out_ptrs[2] = { rev_out_l_.data(), rev_out_r_.data() };
            IPLAudioBuffer rev_out_buf{ 2, frame_, rev_out_ptrs };
            IPLAmbisonicsDecodeEffectParams dparams{};
            dparams.order = kAmbisonicsOrder;
            dparams.hrtf = ipl_hrtf_;
            dparams.orientation = a_listener_;
            dparams.binaural = hrtf_on_.load(std::memory_order_relaxed) ? IPL_TRUE : IPL_FALSE;
            iplAmbisonicsDecodeEffectApply(ambi_decode_, &dparams, &ambi_buf, &rev_out_buf);
            float loudest = 0.0f;
            for (int s = 0; s < frame_; ++s) {
                const float l = std::fabs(rev_out_l_[s]), r = std::fabs(rev_out_r_[s]);
                if (!(l <= loudest)) loudest = l; // also takes over a NaN ...
                if (!(r <= loudest)) loudest = r;
                if (!(loudest < kReverbFault)) break; // ... and stops there
            }
            const bool fault = !(loudest < kReverbFault);
            if (fault) {
                iplReflectionEffectReset(refl_effect_);
                iplAmbisonicsDecodeEffectReset(ambi_decode_);
                reverb_faults_.fetch_add(1, std::memory_order_relaxed);
            }
            const float wet_step = wet * (room1 - room0) / static_cast<float>(frame_);
            for (int s = 0; s < frame_ && !fault; ++s) {
                const float w = wet * room0 + wet_step * static_cast<float>(s + 1);
                mix_l_[s] += rev_out_l_[s] * w;
                mix_r_[s] += rev_out_r_[s] * w;
            }
        }
    }

    if (plan.flush) {
        for (int s = 0; s < frame_; ++s) {
            mix_l_[s] *= env_[s];
            mix_r_[s] *= env_[s];
        }
        for (Voice& v : voices_) v.a_tail_blocks = 0;
        a_glass_entity_ = 0; // the next map's first line starts from nothing (entity numbers come again)
        a_glass_ = 0.0f;
        iplReflectionEffectReset(refl_effect_);
        iplAmbisonicsDecodeEffectReset(ambi_decode_);
        a_room_live_ = false;
        a_reverb_tail_blocks_ = 0;
    }

    // Bass that adds up (combat music, frenzy, disciplines): taken down together before it booms.
    if (tame_loud_.load(std::memory_order_relaxed)) {
        a_bass_guard_.process(mix_l_.data(), mix_r_.data(), frame_, static_cast<float>(rate_), kBassGuard, 1.0f);
        bass_cut_db_.store(a_bass_guard_.cut_db(), std::memory_order_relaxed);
    } else {
        a_bass_guard_.reset();
        bass_cut_db_.store(0.0f, std::memory_order_relaxed);
    }

    // Master gain (ramped), the peak limiter, and the clipper for whatever is left.
    const float target_gain =
        engine_volume_.load(std::memory_order_relaxed) * master_volume_.load(std::memory_order_relaxed);
    const float gain0 = !(a_master_gain_ >= 0.0f) ? target_gain : a_master_gain_; // also never stays NaN
    const float gain1 = gain0 + (target_gain - gain0) * gain_alpha_;
    const float gain_step = (gain1 - gain0) / static_cast<float>(frame_);
    const float gate_ramp = (gate1 - gate0) / static_cast<float>(frame_);
    for (int s = 0; s < frame_; ++s) {
        const float g = (gain0 + gain_step * static_cast<float>(s + 1)) * (gate0 + gate_ramp * static_cast<float>(s + 1));
        carry_[s * 2]     = mix_l_[s] * g;
        carry_[s * 2 + 1] = mix_r_[s] * g;
    }
    a_limiter_.process(carry_.data(), frame_, static_cast<float>(rate_));
    for (float& x : carry_) x = soft_limit(x);
    limiter_db_.store(20.0f * std::log10(a_limiter_.gain()), std::memory_order_relaxed);
    a_master_gain_ = gain1;
    gate_closed_.store(silent, std::memory_order_release); // the block before this one, the end of the fade, is out

    blocks_rendered_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t took = now_us() - t0;
    if (took > render_us_max_.load(std::memory_order_relaxed)) render_us_max_.store(took, std::memory_order_relaxed);
}

uint32_t SteamAudioEngine::buffered_frames() const {
    return write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire);
}

int SteamAudioEngine::voices_in_use() const {
    int n = 0;
    for (const Voice& v : voices_) n += v.state.load(std::memory_order_acquire) != kVoiceFree ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Raytracing thread
// ---------------------------------------------------------------------------------------------------------------

void SteamAudioEngine::run_simulation_sync() {
    run_simulation_step(true);
}

bool SteamAudioEngine::get_channel_direct_params(int ch_idx, IPLDirectEffectParams& out_params) const {
    if (!channel_has_voice(ch_idx)) return false;
    const Voice& v = voices_[slots_[ch_idx].voice];
    SimDirect sim{};
    if (!v.sim.load(sim) || sim.generation != v.generation.load(std::memory_order_relaxed)) return false;
    out_params = IPLDirectEffectParams{};
    out_params.occlusion = sim.occlusion;
    for (int b = 0; b < 3; ++b) out_params.transmission[b] = sim.transmission[b];
    return true;
}

void SteamAudioEngine::sim_thread_main() {
    using clock = std::chrono::steady_clock;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // never at the cost of the game's frame
    clock::time_point next_reflections = clock::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(sim_wake_mutex_);
            sim_wake_.wait_for(lk, std::chrono::milliseconds(16), [this] {
                return sim_kick_ || stop_sim_.load(std::memory_order_acquire);
            });
            sim_kick_ = false;
        }
        if (stop_sim_.load(std::memory_order_acquire)) return;
        if (enabled_.load(std::memory_order_acquire)) {
            const clock::time_point now = clock::now();
            const bool with_reflections = now >= next_reflections;
            if (with_reflections) next_reflections = now + std::chrono::milliseconds(100);
            run_simulation_step(with_reflections);
        }
        Sleep(5); // with the 15.6 ms timer tick: ~32 passes a second, also for a burst of new sounds
    }
}

void SteamAudioEngine::run_simulation_step(bool with_reflections) {
    const EngineConfig cfg = config();
    std::lock_guard<std::mutex> run_lk(sim_run_mutex_);
    if (!ipl_sim_ || !reverb_source_) return;
    {
        std::lock_guard<std::mutex> lk(scene_info_mutex_);
        if (scene_tri_count_ == 0) return;
    }
    ListenerPose pose{};
    if (!listener_pub_.load(pose)) return;

    apply_brush_entities(cfg.dynamic_doors, pose.cs.origin);
    update_sky(pose.cs.origin, with_reflections);

    // Panes of glass in conversations, street sounds in houses.
    for (Voice& v : voices_) {
        VoiceParams params{};
        if (v.state.load(std::memory_order_acquire) == kVoiceFree || v.kind.load(std::memory_order_relaxed) != kMono3D) continue;
        const uint32_t generation = v.generation.load(std::memory_order_acquire); // before the parameters that belong to it
        if (!v.params.load(params) || (params.flags & (kParamDialogue | kParamFreeAmbient)) == 0u) continue;
        place_voice(v, params, generation, cfg, pose.cs.origin);
    }

    // Update simulator source membership & per-source inputs.
    const uint32_t t0 = now_us();
    const uint32_t write = write_pos_.load(std::memory_order_acquire);
    uint32_t generations[kMaxVoices] = {};
    bool membership_changed = false;
    bool any_source = false;
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        VoiceParams params{};
        const bool want_in =
            v.state.load(std::memory_order_acquire) != kVoiceFree &&
            v.kind.load(std::memory_order_relaxed) == kMono3D &&
            frame_diff(write, v.written_end.load(std::memory_order_acquire)) < kDryRate / 2 &&
            v.params.load(params) && (params.flags & kParamNoOcclusion) == 0u;
        generations[i] = v.generation.load(std::memory_order_relaxed);
        if (want_in != v.in_simulator) {
            if (want_in) iplSourceAdd(v.source, ipl_sim_);
            else iplSourceRemove(v.source, ipl_sim_);
            v.in_simulator = want_in;
            membership_changed = true;
        }
        if (!want_in) continue;
        any_source = true;
        IPLSimulationInputs inp{};
        inp.flags = cfg.occlusion_enabled ? IPL_SIMULATIONFLAGS_DIRECT : static_cast<IPLSimulationFlags>(0);
        inp.directFlags = static_cast<IPLDirectSimulationFlags>(
            IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
        inp.source.origin = sky_.leave_solid(leave_own_brush(source_pos_to_ipl(params.origin), params.entnum, pose.cs.origin),
                                             pose.cs.origin, kOcclusionRadius + 0.05f);
        inp.source.ahead  = { 0.0f, 0.0f, -1.0f };
        inp.source.right  = { 1.0f, 0.0f, 0.0f };
        inp.source.up     = { 0.0f, 1.0f, 0.0f };
        inp.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
        inp.occlusionRadius = kOcclusionRadius;
        inp.numOcclusionSamples = std::clamp(cfg.occlusion_samples, 4, 64);
        inp.numTransmissionRays = 2;
        iplSourceSetInputs(v.source, IPL_SIMULATIONFLAGS_DIRECT, &inp);
    }

    // Reflections are traced around the listener so room reverb stays current even when only the
    // player's own footsteps/weapons (2D with a reverb gain) are playing in a quiet room.
    const bool run_reflections = with_reflections && cfg.reverb_enabled;
    if (run_reflections) {
        IPLSimulationInputs inp{};
        inp.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        inp.source = pose.cs;
        inp.reverbScale[0] = inp.reverbScale[1] = inp.reverbScale[2] = 1.0f;
        inp.hybridReverbTransitionTime = static_cast<float>(early_samples_) / static_cast<float>(rate_);
        inp.hybridReverbOverlapPercent = kHybridOverlap;
        iplSourceSetInputs(reverb_source_, IPL_SIMULATIONFLAGS_REFLECTIONS, &inp);
    }

    IPLSimulationSharedInputs shared{};
    shared.listener = pose.cs;
    shared.numRays = std::clamp(cfg.reflection_rays, 256, 4096);
    shared.numBounces = std::clamp(cfg.reflection_bounces, 1, 8);
    shared.duration = std::clamp(cfg.reverb_duration, 0.5f, 3.0f);
    shared.order = kAmbisonicsOrder;
    shared.irradianceMinDistance = 1.0f;
    iplSimulatorSetSharedInputs(ipl_sim_,
        static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS),
        &shared);

    if (membership_changed || scene_dirty_) {
        iplSimulatorCommit(ipl_sim_);
        scene_dirty_ = false;
    }

    if (cfg.occlusion_enabled && any_source) {
        iplSimulatorRunDirect(ipl_sim_);
        for (int i = 0; i < kMaxVoices; ++i) {
            Voice& v = voices_[i];
            if (!v.in_simulator) continue;
            IPLSimulationOutputs out{};
            iplSourceGetOutputs(v.source, IPL_SIMULATIONFLAGS_DIRECT, &out);
            SimDirect sim{};
            sim.generation = generations[i];
            sim.occlusion = out.direct.occlusion;
            for (int b = 0; b < 3; ++b) sim.transmission[b] = out.direct.transmission[b];
            v.sim.store(sim);
        }
    }
    const uint32_t t1 = now_us();
    sim_us_direct_.store(t1 - t0, std::memory_order_relaxed);
    if (run_reflections) {
        iplSimulatorRunReflections(ipl_sim_);
        IPLSimulationOutputs out{};
        iplSourceGetOutputs(reverb_source_, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
        const bool hybrid = reverb_mode_ == ReverbMode::kHybrid;
        out.reflections.type = hybrid ? IPL_REFLECTIONEFFECTTYPE_HYBRID : IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        out.reflections.numChannels = kAmbisonicsChannels;
        if (hybrid) out.reflections.irSize = early_samples_;
        refl_pub_.store(out.reflections);
        for (int b = 0; b < 3; ++b) reverb_time_[b].store(out.reflections.reverbTimes[b], std::memory_order_relaxed);
        refl_valid_.store(true, std::memory_order_release);
        sim_us_reflections_.store(now_us() - t1, std::memory_order_relaxed);
    }
    sim_ticks_.fetch_add(1, std::memory_order_relaxed);
}

// Moves doors, elevators and other brush entities in the scene to where the game has them. Caller holds sim_run_mutex_.
void SteamAudioEngine::apply_brush_entities(bool enabled, IPLVector3 listener) {
    bool dirty = false;
    std::vector<BrushEntityState> states;
    {
        std::lock_guard<std::mutex> lk(brush_mutex_);
        dirty = brush_dirty_;
        brush_dirty_ = false;
        if (dirty && enabled) states = brush_states_;
    }
    const IPLVector3 walked{ listener.x - brush_listener_.x, listener.y - brush_listener_.y, listener.z - brush_listener_.z };
    if (!dirty && ipl_dot(walked, walked) < 1.0f) return;
    brush_listener_ = listener;

    bool changed = dirty && update_brush_instances(states);
    if (select_near_brushes(listener)) changed = true;
    if (!changed) return;

    iplSceneCommit(ipl_scene_);
    scene_dirty_ = true; // the simulator commits before it runs again
    scene_commits_.fetch_add(1, std::memory_order_relaxed);

    sky_movers_.clear();
    glass_movers_.clear();
    std::vector<BrushEntityInfo> infos;
    infos.reserve(brush_instances_.size());
    for (const BrushInstance& inst : brush_instances_) {
        if (!inst.in_scene) continue;
        sky_movers_.push_back(SkyProbe::Mover{ &inst.model->bvh, inst.transform });
        if (!inst.model->glass.empty()) glass_movers_.push_back(GlassProbe::Mover{ &inst.model->glass, inst.transform });
        BrushEntityInfo info{};
        info.entity = inst.state.entity;
        info.model_index = inst.state.model_index;
        for (int k = 0; k < 3; ++k) {
            info.origin[k] = inst.state.origin[k];
            info.angles[k] = inst.state.angles[k];
        }
        infos.push_back(info);
    }
    std::lock_guard<std::mutex> lk(brush_mutex_);
    brush_infos_.swap(infos);
}

// True if something in the scene changed.
bool SteamAudioEngine::update_brush_instances(const std::vector<BrushEntityState>& states) {
    std::unordered_map<int, std::size_t> by_entity;
    by_entity.reserve(brush_instances_.size());
    for (std::size_t i = 0; i < brush_instances_.size(); ++i) {
        brush_instances_[i].seen = false;
        by_entity.emplace(brush_instances_[i].state.entity, i);
    }

    bool changed = false;
    for (const BrushEntityState& s : states) {
        float sum = 0.0f;
        for (int k = 0; k < 3; ++k) sum += s.origin[k] + s.angles[k];
        if (!s.solid || !std::isfinite(sum)) continue;
        const auto it = by_entity.find(s.entity);
        if (it != by_entity.end() && brush_instances_[it->second].state.model_index == s.model_index) {
            BrushInstance& inst = brush_instances_[it->second];
            inst.seen = true;
            float delta = 0.0f;
            for (int k = 0; k < 3; ++k) {
                delta = std::max(delta, std::fabs(s.origin[k] - inst.state.origin[k]));
                delta = std::max(delta, std::fabs(s.angles[k] - inst.state.angles[k]));
            }
            if (delta < kMoveEpsilon) continue;
            inst.state = s;
            inst.transform = source_transform_to_ipl(s.origin, s.angles);
            iplInstancedMeshUpdateTransform(inst.mesh, ipl_scene_, inst.transform);
            brush_moves_.fetch_add(1, std::memory_order_relaxed);
            changed = changed || inst.in_scene;
            continue;
        }
        const SubScene* model = sub_scene(s.model_index);
        if (!model) continue; // nothing in it that stops sound
        BrushInstance inst{};
        inst.state = s;
        inst.model = model;
        inst.transform = source_transform_to_ipl(s.origin, s.angles);
        inst.seen = true;
        IPLInstancedMeshSettings settings{};
        settings.subScene = model->scene;
        settings.transform = inst.transform;
        if (iplInstancedMeshCreate(ipl_scene_, &settings, &inst.mesh) != IPL_STATUS_SUCCESS || !inst.mesh) continue;
        brush_instances_.push_back(inst);
    }

    for (std::size_t i = 0; i < brush_instances_.size();) {
        BrushInstance& inst = brush_instances_[i];
        if (inst.seen) {
            ++i;
            continue;
        }
        if (inst.in_scene) {
            iplInstancedMeshRemove(inst.mesh, ipl_scene_);
            changed = true;
        }
        iplInstancedMeshRelease(&inst.mesh);
        inst = brush_instances_.back();
        brush_instances_.pop_back();
    }
    return changed;
}

bool SteamAudioEngine::select_near_brushes(IPLVector3 listener) {
    brush_order_.clear();
    for (std::size_t i = 0; i < brush_instances_.size(); ++i) {
        const BrushInstance& inst = brush_instances_[i];
        const IPLVector3 lo = inst.model->bvh.bounds_min(), hi = inst.model->bvh.bounds_max();
        // Distance to the box, in its own space.
        const IPLVector3 p = inverse_transform_point(inst.transform, listener);
        const float dx = std::max(0.0f, std::max(lo.x - p.x, p.x - hi.x));
        const float dy = std::max(0.0f, std::max(lo.y - p.y, p.y - hi.y));
        const float dz = std::max(0.0f, std::max(lo.z - p.z, p.z - hi.z));
        brush_order_.emplace_back(std::sqrt(dx * dx + dy * dy + dz * dz), i);
    }
    std::sort(brush_order_.begin(), brush_order_.end());
    bool changed = false;
    for (std::size_t rank = 0; rank < brush_order_.size(); ++rank) {
        BrushInstance& inst = brush_instances_[brush_order_[rank].second];
        const float dist = brush_order_[rank].first;
        const bool want = inst.in_scene ? (rank < kMaxBrushesLeave && dist < kBrushRangeLeave)
                                        : (rank < kMaxBrushes && dist < kBrushRange);
        if (want == inst.in_scene) continue;
        if (want) iplInstancedMeshAdd(inst.mesh, ipl_scene_);
        else iplInstancedMeshRemove(inst.mesh, ipl_scene_);
        inst.in_scene = want;
        changed = true;
    }
    return changed;
}

// The geometry of brush model "*index", built when an entity first uses it. nullptr: nothing in it stops sound.
const SteamAudioEngine::SubScene* SteamAudioEngine::sub_scene(int model_index) {
    for (const auto& s : sub_scenes_) {
        if (s->model_index == model_index) return s->scene ? s.get() : nullptr;
    }
    auto sub = std::make_unique<SubScene>();
    sub->model_index = model_index;
    for (const AcousticSubModel& m : brush_models_) {
        if (m.model_index != model_index || m.triangles.empty() || brush_materials_.empty()) continue;
        sub->bvh.build(m.vertices, m.triangles, m.triangles.size());
        sub->glass.build(m.glass_vertices, m.glass_triangles, m.glass_triangles.size());
        const IPLVector3 lo = sub->bvh.bounds_min(), hi = sub->bvh.bounds_max();
        float extent[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
        std::sort(extent, extent + 3);
        if (extent[1] < kMinBrushExtent) break;
        IPLSceneSettings scene_cfg{};
        scene_cfg.type = IPL_SCENETYPE_DEFAULT;
        if (iplSceneCreate(ipl_ctx_, &scene_cfg, &sub->scene) != IPL_STATUS_SUCCESS || !sub->scene) break;
        IPLStaticMeshSettings mcfg{};
        mcfg.numVertices = static_cast<IPLint32>(m.vertices.size());
        mcfg.numTriangles = static_cast<IPLint32>(m.triangles.size());
        mcfg.numMaterials = static_cast<IPLint32>(brush_materials_.size());
        mcfg.vertices = const_cast<IPLVector3*>(m.vertices.data());
        mcfg.triangles = const_cast<IPLTriangle*>(m.triangles.data());
        mcfg.materialIndices = const_cast<IPLint32*>(m.material_indices.data());
        mcfg.materials = brush_materials_.data();
        if (iplStaticMeshCreate(sub->scene, &mcfg, &sub->mesh) != IPL_STATUS_SUCCESS || !sub->mesh) {
            iplSceneRelease(&sub->scene);
            sub->scene = nullptr;
            break;
        }
        iplStaticMeshAdd(sub->mesh, sub->scene);
        iplSceneCommit(sub->scene);
        break;
    }
    sub_scenes_.push_back(std::move(sub));
    return sub_scenes_.back()->scene ? sub_scenes_.back().get() : nullptr;
}

// The caller commits the scene.
void SteamAudioEngine::release_brush_instances() {
    for (BrushInstance& inst : brush_instances_) {
        if (!inst.mesh) continue;
        if (ipl_scene_ && inst.in_scene) iplInstancedMeshRemove(inst.mesh, ipl_scene_);
        iplInstancedMeshRelease(&inst.mesh);
    }
    brush_instances_.clear();
    sky_movers_.clear();
    glass_movers_.clear();
}

void SteamAudioEngine::release_sub_scenes() {
    for (auto& sub : sub_scenes_) {
        if (sub->mesh && sub->scene) iplStaticMeshRemove(sub->mesh, sub->scene);
        if (sub->mesh) iplStaticMeshRelease(&sub->mesh);
        if (sub->scene) iplSceneRelease(&sub->scene);
    }
    sub_scenes_.clear();
}

// A door's own sound comes from the middle of the door. Its occlusion is traced from where the line to the listener
// leaves the door, so that the door does not muffle itself.
IPLVector3 SteamAudioEngine::leave_own_brush(IPLVector3 source, int entnum, IPLVector3 listener) const {
    for (const BrushInstance& inst : brush_instances_) {
        if (inst.state.entity != entnum || !inst.in_scene) continue;
        const IPLVector3 p = inverse_transform_point(inst.transform, source);
        const IPLVector3 q = inverse_transform_point(inst.transform, listener);
        const IPLVector3 lo = inst.model->bvh.bounds_min(), hi = inst.model->bvh.bounds_max();
        const float pp[3] = { p.x, p.y, p.z }, qq[3] = { q.x, q.y, q.z };
        const float ll[3] = { lo.x, lo.y, lo.z }, hh[3] = { hi.x, hi.y, hi.z };
        float exit = 1.0f;
        for (int a = 0; a < 3; ++a) {
            if (pp[a] < ll[a] - 0.02f || pp[a] > hh[a] + 0.02f) return source; // not inside
            const float d = qq[a] - pp[a];
            if (d > 1e-6f) exit = std::min(exit, (hh[a] - pp[a]) / d);
            else if (d < -1e-6f) exit = std::min(exit, (ll[a] - pp[a]) / d);
        }
        const IPLVector3 dir{ listener.x - source.x, listener.y - source.y, listener.z - source.z };
        const float len = std::sqrt(ipl_dot(dir, dir));
        if (len < 1e-3f) return source;
        const float t = std::min(0.95f, std::max(0.0f, exit) + (kOcclusionRadius + 0.05f) / len);
        return IPLVector3{ source.x + dir.x * t, source.y + dir.y * t, source.z + dir.z * t };
    }
    return source;
}

void SteamAudioEngine::update_sky(IPLVector3 listener, bool now) {
    if (!now && sky_countdown_ > 0) {
        --sky_countdown_;
        return;
    }
    sky_countdown_ = 3; // ~10 Hz
    sky_exposure_.store(sky_.exposure(listener, sky_movers_.data(), sky_movers_.size()), std::memory_order_relaxed);
}

void SteamAudioEngine::place_voice(Voice& v, const VoiceParams& params, uint32_t generation, const EngineConfig& cfg,
                                   IPLVector3 listener) {
    const IPLVector3 source = source_pos_to_ipl(params.origin);
    SimPlace place{};
    place.generation = generation;
    if ((params.flags & kParamDialogue) != 0u) {
        // All the time: the first rays may be older than the camera's cut to the conversation.
        if (cfg.dialogue_glass) {
            place.glass = glass_.between(listener, source, glass_movers_.data(), glass_movers_.size());
            PlayerHead head{};
            if (player_pub_.load(head) && head.known) {
                place.glass = std::max(place.glass, glass_.between(head.position, source, glass_movers_.data(), glass_movers_.size()));
            }
        }
        v.place.store(place);
        return;
    }
    if (v.s_placed == generation) return; // it stays where it was put
    v.s_placed = generation;
    if (sky_.has_sky() && sky_exposure_.load(std::memory_order_relaxed) >= kOutdoorExposure) {
        place.position = sky_.open_direction(listener, source);
        const IPLVector3 moved{ place.position.x - source.x, place.position.y - source.y, place.position.z - source.z };
        place.relocated = ipl_dot(moved, moved) > 0.01f;
    }
    v.place.store(place);
}

std::vector<VoiceInfo> SteamAudioEngine::voice_infos() const {
    std::vector<VoiceInfo> infos;
    for (int i = 0; i < kMaxVoices; ++i) {
        const Voice& v = voices_[i];
        if (v.state.load(std::memory_order_acquire) == kVoiceFree) continue;
        VoiceInfo info{};
        info.voice = i;
        for (int c = 0; c < kMaxChannels; ++c) {
            if (slots_[c].voice == i) info.slot = c;
        }
        VoiceParams p{};
        if (!v.params.load(p)) continue;
        info.spatial_3d = v.kind.load(std::memory_order_relaxed) == kMono3D;
        info.is_voice = (p.flags & kParamVoice) != 0u;
        info.dialogue = (p.flags & kParamDialogue) != 0u;
        info.no_occlusion = (p.flags & kParamNoOcclusion) != 0u;
        info.directional = info.spatial_3d && (p.flags & kParamDirectional) != 0u;
        if (info.directional) info.directivity = v.dbg_directivity.load(std::memory_order_relaxed);
        info.master_gain = p.master_gain;
        info.weather = p.weather;
        info.distance_m = v.dbg_distance.load(std::memory_order_relaxed);
        info.attenuation = v.dbg_atten.load(std::memory_order_relaxed);
        info.occlusion = v.dbg_occ.load(std::memory_order_relaxed);
        info.reverb_send = v.dbg_send.load(std::memory_order_relaxed);
        info.weather_gain = v.dbg_weather_gain.load(std::memory_order_relaxed);
        info.level_db = v.dbg_level.load(std::memory_order_relaxed);
        info.recorded_db = v.dbg_recorded.load(std::memory_order_relaxed);
        info.outdoor_ambient = (p.flags & kParamOutdoor) != 0u;
        if (info.dialogue) info.glass = v.dbg_glass.load(std::memory_order_relaxed);
        SimPlace place{};
        info.relocated = info.spatial_3d && (p.flags & kParamFreeAmbient) != 0u && v.place.load(place) && place.relocated &&
                         place.generation == v.generation.load(std::memory_order_relaxed);
        SimDirect sim{};
        if (info.spatial_3d && !info.no_occlusion && v.sim.load(sim) &&
            sim.generation == v.generation.load(std::memory_order_relaxed)) {
            for (int b = 0; b < 3; ++b) info.transmission[b] = sim.transmission[b];
        }
        infos.push_back(info);
    }
    return infos;
}

std::vector<BrushEntityInfo> SteamAudioEngine::brush_entity_infos() const {
    std::lock_guard<std::mutex> lk(brush_mutex_);
    return brush_infos_;
}

EngineStats SteamAudioEngine::get_stats() const {
    EngineStats s{};
    s.initialized = initialized_.load(std::memory_order_acquire);
    s.device_started = device_started_.load(std::memory_order_relaxed);
    s.enabled = enabled_.load(std::memory_order_acquire);
    s.active_3d_channels = active_3d_count_.load(std::memory_order_relaxed);
    s.active_2d_channels = active_2d_count_.load(std::memory_order_relaxed);
    s.blocks_rendered = blocks_rendered_.load(std::memory_order_relaxed);
    s.sim_ticks = sim_ticks_.load(std::memory_order_relaxed);
    s.dry_buffered_frames = buffered_frames();
    s.engine_ahead_frames = static_cast<uint32_t>(engine_ahead_.load(std::memory_order_relaxed));
    s.output_rate = static_cast<uint32_t>(output_rate());
    s.device_rate = s.output_rate;
    s.frame_size = static_cast<uint32_t>(frame_size());
    if (const uint32_t device_rate = dev_rate_.load(std::memory_order_relaxed)) {
        s.device_rate = device_rate;
        s.device_period_frames = dev_period_.load(std::memory_order_relaxed);
        s.device_buffer_frames = dev_buffer_.load(std::memory_order_relaxed);
    }
    s.focused = focused_.load(std::memory_order_relaxed);
    s.game_paused = game_paused_.load(std::memory_order_relaxed);
    s.held = held_.load(std::memory_order_relaxed);
    s.silent = gate_closed_.load(std::memory_order_relaxed);
    s.device_changes = device_changes_.load(std::memory_order_relaxed);
    s.reverb_faults = reverb_faults_.load(std::memory_order_relaxed);
    s.dsp_rebuilds = dsp_rebuilds_.load(std::memory_order_relaxed);
    s.rebuild_pending = rebuild_pending_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        s.device_name = device_name_;
        s.hrtf_name = hrtf_info_.name;
        s.hrtf_warning = hrtf_info_.warning;
    }
    s.queued_frames_avg = queued_avg_.load(std::memory_order_relaxed);
    s.queued_frames_max = queued_max_.exchange(0, std::memory_order_relaxed);
    s.reserve_frames = reserve_.load(std::memory_order_relaxed);
    s.late_wakes = late_wakes_.load(std::memory_order_relaxed);
    s.voices = static_cast<uint32_t>(voices_in_use());
    s.pose_age_us = pose_age_us_.load(std::memory_order_relaxed);
    s.render_us_max = render_us_max_.exchange(0, std::memory_order_relaxed);
    s.starvations = starvations_.load(std::memory_order_relaxed);
    s.dropped_chunks = dropped_chunks_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    for (int b = 0; b < 3; ++b) s.reverb_time[b] = reverb_time_[b].load(std::memory_order_relaxed);
    s.reverb_mode = static_cast<ReverbMode>(pub_reverb_mode_.load(std::memory_order_relaxed));
    s.sky_exposure = sky_exposure_.load(std::memory_order_relaxed);
    s.rain = rain_level_.load(std::memory_order_relaxed);
    s.bass_cut_db = bass_cut_db_.load(std::memory_order_relaxed);
    s.limiter_db = limiter_db_.load(std::memory_order_relaxed);
    s.brush_moves = brush_moves_.load(std::memory_order_relaxed);
    s.scene_commits = scene_commits_.load(std::memory_order_relaxed);
    s.sim_us_direct = sim_us_direct_.load(std::memory_order_relaxed);
    s.sim_us_reflections = sim_us_reflections_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(brush_mutex_);
        s.brush_entities = static_cast<uint32_t>(brush_infos_.size());
    }
    {
        std::lock_guard<std::mutex> lk(scene_info_mutex_);
        s.has_scene = (scene_tri_count_ > 0);
        s.scene_triangles = scene_tri_count_;
        s.brush_models = brush_model_count_;
        s.map_has_sky = has_sky_;
        s.glass_triangles = glass_tri_count_;
        s.loaded_map = loaded_map_;
    }
    return s;
}

} // namespace sa
