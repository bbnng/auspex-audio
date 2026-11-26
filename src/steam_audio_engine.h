#pragma once

#include "bsp_acoustics.h"
#include "channel_model.h"
#include "coord_util.h"
#include "dynamics.h"
#include "engine_config.h"
#include "lockfree.h"
#include "output_pacer.h"
#include "resampler.h"
#include "sky_exposure.h"

#include <phonon.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ma_device;

namespace sa {

class DefaultDeviceWatch;

// How audio flows:
//
//   game main thread   Mix* hooks -> dry (unspatialized) PCM per voice, on one timeline that follows paintedtime.
//                      S_Update   -> listener pose + every channel's position/volume, once per rendered frame.
//   WASAPI callback    Waits until the device is about to run dry, then reads the dry PCM that is due, resamples it to
//                      the device's own rate and applies direct path + HRTF + reverb with the pose and positions
//                      published MOST RECENTLY. Nothing spatial is ever stored in a buffer, so snd_mixahead (0.2 s in
//                      VTMB's config.cfg), hitches and backlog cannot delay a camera turn.
//   raytracing thread  Doors and other moving brushes, occlusion/transmission per voice (~30 Hz), sky above the listener
//                      and the room's reflections (~10 Hz).
//   device thread      Opens the output device and follows it: another default device, one that was unplugged, stopped or
//                      changed its format. If the new one runs at another rate, or a setting needs other Steam Audio
//                      objects (HRTF file, reverb mode), it stops the callback, builds them anew and starts it again.
//
// The callback takes no locks and allocates nothing. Everything it uses is only ever replaced while it does not run.

constexpr int kDryRate     = 44100; // engine.dll paints at 44.1 kHz; the dry timeline counts these frames
constexpr int kDefaultFrameSize = 128; // DSP block: 2.7 ms at 48 kHz. Devices get the divisor of their period next to it
constexpr int kMinFrameSize = 96;
constexpr int kMaxFrameSize = 160;
constexpr int kMaxChannels = 128; // engine.dll channel_t slots
constexpr int kMaxVoices   = 160; // one per playing sound, plus sounds whose slot was reused but that are still audible
constexpr int kMaxChunk    = 2048;

// A centred sound has the same loudness as 2D and as 3D in engine.dll. The HRTF's two ears together are 1.1 to 1.9 dB
// quieter for a source ahead than plain left = right (measured, Steam Audio's default HRTF, broadband and low-passed):
// 2D sounds give way by that much.
constexpr float kLevel2D = 0.84f;

// MIX_MixChannels stops mixing a looping channel once all its volumes (master_vol * gain * pan, pan 0.5 for a centred
// source) are below 6 of 255. Loops fade to silence exactly there, so the cut is never heard.
constexpr float kCullLevel = 12.0f / 255.0f;

// The divisor of a device period next to kDefaultFrameSize: no rendered frames are left over between two callbacks.
int block_for_period(uint32_t period);

// A brush entity (door, elevator, func_brush, breakable...) as the client has it this frame.
struct BrushEntityState {
    int   entity = 0;
    int   model_index = 0; // N of "*N"
    float origin[3] = {};
    float angles[3] = {}; // pitch, yaw, roll
    bool  solid = false;
};

struct BrushEntityInfo { // sa_debug
    int   entity = 0;
    int   model_index = 0;
    float origin[3] = {};
    float angles[3] = {};
};

struct VoiceInfo { // sa_debug
    int   voice = -1;
    int   slot = -1; // channel_t index, -1: the engine already freed it
    bool  spatial_3d = false;
    bool  is_voice = false;
    bool  dialogue = false;
    bool  no_occlusion = false;
    bool  directional = false;
    float directivity = 1.0f;  // how much of the direct sound the character's facing leaves (broadband)
    float distance_m = 0.0f;
    float master_gain = 0.0f;
    float attenuation = 0.0f;
    float occlusion = 1.0f;
    float transmission[3] = { 1.0f, 1.0f, 1.0f };
    float reverb_send = 0.0f;
    float weather = 0.0f;
    float weather_gain = 1.0f;
    float glass = 0.0f;        // 1: a conversation through a pane of glass
    float level_db = 0.0f;     // what the leveler adds to (or takes off) the recording
    float recorded_db = -120.0f; // how loud a voice was recorded, as far as it was heard (-120: unknown)
    bool  outdoor_ambient = false;
    bool  relocated = false;   // a street sound, taken out of the houses
};

struct EngineStats {
    bool     initialized = false;
    bool     device_started = false;
    bool     enabled = false;
    bool     has_scene = false;
    uint32_t scene_triangles = 0;
    uint32_t active_3d_channels = 0;
    uint32_t active_2d_channels = 0;
    uint64_t blocks_rendered = 0;
    uint64_t sim_ticks = 0;
    uint32_t dry_buffered_frames = 0; // dry PCM waiting for playback (does not delay direction)
    uint32_t engine_ahead_frames = 0; // paintedtime - soundtime as engine.dll reports it
    // Output path. Rates in Hz, frames at output_rate unless noted.
    uint32_t output_rate = 0;         // what Steam Audio renders and the device stream runs at
    uint32_t device_rate = 0;         // the device's own (shared mode mix) rate
    uint32_t frame_size = 0;
    uint32_t device_period_frames = 0; // device_rate frames
    uint32_t device_buffer_frames = 0; // device_rate frames
    uint32_t queued_frames_avg = 0;   // device_rate frames still queued in WASAPI when a period was rendered
    uint32_t queued_frames_max = 0;   // since the last get_stats()
    uint32_t reserve_frames = 0;      // a period is rendered once no more than this is queued
    uint32_t late_wakes = 0;          // the callback came too late (reserve was raised)
    uint32_t pose_age_us = 0;         // age of the listener pose when the last block used it
    uint32_t render_us_max = 0;       // worst block render time since the last get_stats()
    uint32_t starvations = 0;         // dry PCM ran out (game stalled longer than snd_mixahead)
    uint32_t dropped_chunks = 0;
    uint32_t resyncs = 0;             // jumps back onto the engine's playback clock
    uint32_t voices = 0;
    float    reverb_time[3] = {};     // simulated RT60 (low, mid, high)
    ReverbMode reverb_mode = ReverbMode::kParametric;
    float    sky_exposure = 1.0f;     // 1 = open sky above the listener, 0 = roof
    float    rain = 0.0f;             // 0..1: how much rain covers other sounds where the listener stands
    float    bass_cut_db = 0.0f;      // what the bass guard takes off the sum right now
    float    limiter_db = 0.0f;       // and the peak limiter
    uint32_t glass_triangles = 0;     // of the map's panes
    bool     map_has_sky = false;
    uint32_t brush_models = 0;        // brush models of the map that can occlude
    uint32_t brush_entities = 0;      // entities with such a model that stand in the scene now
    uint32_t brush_moves = 0;         // transforms updated so far
    uint32_t scene_commits = 0;
    uint32_t sim_us_direct = 0;       // last step
    uint32_t sim_us_reflections = 0;
    bool     focused = true;          // the game's window is in front
    bool     game_paused = false;
    bool     held = false;            // the engine stopped its sound device (saving, system menu)
    bool     silent = false;          // faded out completely (in the background)
    uint32_t device_changes = 0;      // output device opened again
    uint32_t dsp_rebuilds = 0;        // Steam Audio objects built anew
    uint32_t reverb_faults = 0;       // blocks whose reverb was no sound at all and was left out
    bool     rebuild_pending = false;
    std::string device_name;
    std::string hrtf_name;            // "default" or the .sofa file in use
    std::string hrtf_warning;         // why a wished .sofa file is not in use
    std::string loaded_map;
};

class SteamAudioEngine {
public:
    SteamAudioEngine();
    ~SteamAudioEngine();

    SteamAudioEngine(const SteamAudioEngine&) = delete;
    SteamAudioEngine& operator=(const SteamAudioEngine&) = delete;

    // Initialize Steam Audio context, HRTF, simulator, effects, and (unless headless) miniaudio WASAPI device.
    bool init(const EngineConfig& cfg, bool headless, std::string& out_error);
    void shutdown();

    bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }
    bool is_enabled() const { return enabled_.load(std::memory_order_acquire); }
    void set_enabled(bool on);

    EngineConfig config() const;
    void set_master_volume(float vol);
    void set_hrtf_enabled(bool on);
    void set_occlusion_enabled(bool on);
    void set_reverb_enabled(bool on);
    void set_reverb_wet(float wet);

    void set_dynamic_doors(bool on);
    void set_weather_occlusion(bool on);
    void set_weather_volume(float volume);
    void set_rain_masking(float strength);
    void set_dialogue_glass(bool on);
    // [Tweaks]. Those about single channels are applied by the caller (channel_tweaks()).
    void set_tweaks(const EngineConfig& cfg);
    ChannelTweaks channel_tweaks() const;
    void set_voice_directivity(bool on);
    void set_mute_in_background(bool on);
    // All of the settings above at once (presets).
    void apply_settings(const EngineConfig& cfg);

    // These need other Steam Audio objects. The device thread fades out, builds them and fades in again; a message says
    // how it went. Without a device thread (headless) it has happened when the call returns.
    void request_reverb_mode(ReverbMode mode);
    // An empty file is the built-in HRTF. A file that cannot be loaded changes nothing.
    void request_hrtf(const std::string& sofa_file, const std::wstring& sofa_path);
    // Opens the output device anew, as when Windows reports another one.
    void reopen_output();
    // What the device thread has to say (device changes, results), for the log. Game main thread.
    bool pop_message(std::string& out);

    // Load or unload the map (static BSP world geometry, brush models, sky) into the Steam Audio raytracing scene.
    bool load_scene(const AcousticMeshData& mesh, std::string& out_summary);
    void unload_scene();

    // ---- Game main thread ----

    // Publish the listener pose (Source coordinates & basis vectors). The very next DSP block uses it.
    // Zero vectors (the engine's "no view" listener in menus) are ignored.
    void update_listener(
        const float origin[3],
        const float forward[3],
        const float right[3],
        const float up[3]);

    // Where the player's head is (Source coordinates), or nullptr if that is not known. In a conversation the camera
    // cuts around, but a pane between the player and the other one stays there.
    void update_player_head(const float position[3]);

    // Publish a playing channel's live position/volume. Applies to audio that is already buffered.
    // Ignored if the slot has no voice or plays another sound now.
    void update_channel(int ch_idx, const ChannelDescriptor& desc);

    // Publish where all brush entities are. Call when any of them changed; the raytracing thread moves them.
    void update_brush_entities(const BrushEntityState* states, std::size_t count);

    // The engine's distance model and `volume` cvar.
    void set_gain_model(const GainModel& model);
    void set_engine_volume(float volume);

    // How many frames engine.dll has painted beyond its playback position (paintedtime - soundtime).
    void set_engine_ahead(int frames);

    // The game's window lost or got the focus: everything fades out (DirectSound muted the original game by itself) and
    // in again. Time goes on meanwhile, as in the original.
    void set_focused(bool focused);
    // The game is paused: what plays on (menu) plays, what was playing has stopped, and so does the room's reverb.
    void set_game_paused(bool paused);
    // The engine stopped its sound device and with it its clock (while it saves): what is buffered waits.
    void set_held(bool held);

    // Begin a paint chunk of up to `chunk_frames` samples (at kDryRate).
    void begin_chunk(int chunk_frames);

    // Feed dry mono or stereo float samples for channel `ch_idx` into the current chunk.
    void feed_channel_mono(int ch_idx, const ChannelDescriptor& desc,
                           const float* mono_samples, int offset_in_chunk, int count);
    void feed_channel_stereo(int ch_idx, const ChannelDescriptor& desc,
                             const float* left_samples, const float* right_samples,
                             int offset_in_chunk, int count);

    // Commit the current chunk to the timeline. If `actual_chunk_frames > 0`, trims it to that sample count.
    void end_chunk(int actual_chunk_frames = -1);

    // Throw the current chunk away (catch-up audio whose playback time is already over).
    void discard_chunk();

    // The engine freed a channel slot. Audio the engine had already mixed for it still plays out.
    void notify_channel_freed(int ch_idx);
    bool channel_has_voice(int ch_idx) const;

    // Fade out and drop everything buffered (level unload, engine restart, disable).
    void flush_audio();

    // ---- Audio thread (miniaudio callback), or the caller in headless mode ----

    // Render exactly `frames` interleaved stereo frames (at output_rate()) with the latest published pose.
    void render_output(float* out_interleaved_lr, uint32_t frames);
    // Waits until the WASAPI buffer is about to run dry, so that what is rendered next is heard at once.
    void pace_output(ma_device* dev);
    void count_callback() { callbacks_.fetch_add(1, std::memory_order_relaxed); }

    // ---- Headless test helpers ----

    // Run one synchronous simulation step (occlusion and reverb) immediately.
    void run_simulation_sync();
    // What happens when the output device is replaced by one with this rate and period.
    bool change_output_sync(int rate, uint32_t period, std::string& out_error);
    // Query the last simulated direct outputs for a channel.
    bool get_channel_direct_params(int ch_idx, IPLDirectEffectParams& out_params) const;
    uint32_t buffered_frames() const;
    int voices_in_use() const;

    // Simulation results for tests.
    float sky_exposure() const { return sky_exposure_.load(std::memory_order_relaxed); }
    float rain() const { return rain_level_.load(std::memory_order_relaxed); }

    int output_rate() const { return pub_rate_.load(std::memory_order_relaxed); }
    int frame_size() const { return pub_frame_.load(std::memory_order_relaxed); }

    EngineStats get_stats() const;
    // Snapshots for sa_debug (game main thread).
    std::vector<VoiceInfo> voice_infos() const;
    std::vector<BrushEntityInfo> brush_entity_infos() const;

private:
    enum VoiceState : uint32_t { kVoiceFree = 0, kVoiceActive = 1, kVoiceDraining = 2 };
    enum VoiceKind : uint32_t { kMono3D = 0, kMono2D = 1, kStereo2D = 2 };

    struct ListenerPose {
        IPLCoordinateSpace3 cs;
        uint32_t stamp_us;
    };

    struct VoiceParams {
        float origin[3]; // Source units
        float dist_mult;
        float radius;
        float master_gain;
        float loop_master;
        float reverb_gain;
        float weather;
        float rain;
        float forward[3]; // Source axes, kParamDirectional
        int32_t entnum;
        uint32_t flags; // kParam*
        uint32_t voice_file;
        uint32_t voice_speaker;
        Leveling leveling;
    };
    static constexpr uint32_t kParamVoice = 1;
    static constexpr uint32_t kParamDialogue = 2;
    static constexpr uint32_t kParamNoOcclusion = 4;
    static constexpr uint32_t kParamDirectional = 8;
    static constexpr uint32_t kParamOutdoor = 16;
    static constexpr uint32_t kParamFreeAmbient = 32;

    struct PlayerHead {
        IPLVector3 position;
        bool known;
    };

    // What the raytracing thread found out about one sound apart from walls.
    struct SimPlace {
        uint32_t generation;
        float glass;       // conversations: 0..1
        bool  relocated;   // street sounds: `position` instead of the game's
        IPLVector3 position;
    };

    struct SimDirect {
        uint32_t generation;
        float occlusion;
        float transmission[3];
    };

    // One playing sound. A voice outlives its channel_t slot: engine.dll mixes snd_mixahead seconds early, so when
    // it frees a slot up to 200 ms of that sound are still to be heard (at that sound's own position).
    struct Voice {
        // Fences. Only the main thread makes a voice Active/Draining, only the audio thread makes it Free.
        std::atomic<uint32_t> state{kVoiceFree};
        std::atomic<uint32_t> generation{0};
        std::atomic<uint32_t> kind{kMono3D};
        std::atomic<uint32_t> start_pos{0};
        std::atomic<uint32_t> written_end{0};
        SeqLock<VoiceParams>  params;
        SeqLock<SimDirect>    sim;
        SeqLock<SimPlace>     place;
        DryRing ring_l;
        DryRing ring_r;

        // Main thread only.
        bool     p_has_data = false;
        bool     p_fed_this_chunk = false;
        uint32_t p_fade_done = 0;

        // Audio thread only.
        IPLDirectEffect   direct_effect   = nullptr;
        IPLBinauralEffect binaural_effect = nullptr;
        uint32_t a_generation = 0;
        bool     a_smooth_init = false; // false: snap every smoothed value to its target
        bool     a_reset_fx = true;
        int      a_tail_blocks = 0;
        VoiceParams a_params{};
        IPLVector3 a_pos{};
        float a_atten = 1.0f;
        float a_occ = 1.0f;
        float a_trans[3] = { 1.0f, 1.0f, 1.0f };
        float a_master = 1.0f;
        float a_reverb = 0.0f;
        float a_pan_l = 0.0f, a_pan_r = 0.0f;
        VoiceDirectivity a_dir{};
        float a_weather = 1.0f;    // smoothed sky exposure this sound is played with
        float a_lp[2][2] = {};     // weather low-pass: two one-pole stages per side
        float a_cover = 0.0f;      // smoothed: how much rain covers this 2D sound
        float a_glass = 0.0f;
        LoudnessLeveler a_leveler;
        VoiceLeveler a_voice;
        bool     a_voice_new = true;   // nothing of this sound has been listened to
        uint32_t a_listened = 0;       // how far on the dry timeline it has been
        float    a_voice_ahead = 0.0f; // seconds that is ahead of what is played

        // What the audio thread last did, for sa_debug.
        std::atomic<float> dbg_distance{0.0f};
        std::atomic<float> dbg_atten{0.0f};
        std::atomic<float> dbg_occ{1.0f};
        std::atomic<float> dbg_send{0.0f};
        std::atomic<float> dbg_weather_gain{1.0f};
        std::atomic<float> dbg_directivity{1.0f};
        std::atomic<float> dbg_glass{0.0f};
        std::atomic<float> dbg_level{0.0f};
        std::atomic<float> dbg_recorded{-120.0f};

        // Raytracing thread only.
        IPLSource source = nullptr;
        bool      in_simulator = false;
        uint32_t  s_placed = 0;    // the generation whose place has been decided
    };

    struct Slot { // main thread only
        int  voice = -1;
        int  entnum = 0;
        int  entchannel = 0;
        std::uintptr_t mixer = 0;
    };

    struct BlockPlan {
        uint32_t read = 0;     // first timeline frame of this block
        double   frac = 0.0;   // position between two dry frames where the block starts
        double   step = 1.0;   // dry frames per output frame (44100 / output rate, +-0.4 % drift correction)
        uint32_t frames = 0;   // readable frames from `read` on (includes the resampler's look-ahead)
        uint32_t advance = 0;  // how far the timeline moves
        uint32_t write = 0;    // committed end of the timeline
        double   next_frac = 0.0;
        bool     direct = false; // 1:1, no resampling
        bool     use_env = false;
        bool     flush = false; // env_ fades the whole output, then everything is reset
    };

    // Raytracing thread. A brush model's geometry, instanced at every entity that uses it.
    struct SubScene {
        int model_index = 0;
        IPLScene scene = nullptr;
        IPLStaticMesh mesh = nullptr;
        TriangleBvh bvh;
        TriangleBvh glass; // its panes only
    };
    struct BrushInstance {
        BrushEntityState state;
        const SubScene* model = nullptr;
        IPLInstancedMesh mesh = nullptr;
        IPLMatrix4x4 transform{};
        bool seen = false;
        bool in_scene = false;
    };

    // Main thread.
    int  voice_for_feed(int ch_idx, const ChannelDescriptor& desc, VoiceKind kind);
    void retire_slot(int ch_idx);
    void publish_params(Voice& v, const ChannelDescriptor& desc);
    void feed_voice(int voice_idx, const float* left, const float* right, int offset, int count);
    void trim_fed_voices(uint32_t limit);

    // Audio thread.
    void render_block();
    BlockPlan plan_block();
    void set_env_fade_out(int frames);
    void set_env_fade_in();
    void fetch_dry(const Voice& v, const DryRing& ring, const BlockPlan& plan, float* dst);
    void render_voice(Voice& v, const BlockPlan& plan, bool& any_reverb_input);
    bool send_to_reverb(const float* mono, float gain0, float gain1);
    void apply_weather(Voice& v, int side, float* samples, float gain0, float gain1, float coeff0, float coeff1);
    void listen_ahead(Voice& v, const BlockPlan& plan, uint32_t kind);
    void remember_voice(Voice& v);
    void level_voice(Voice& v, float* left, float* right, float scale, bool first);
    void update_rain();

    // Raytracing thread.
    void sim_thread_main();
    void run_simulation_step(bool with_reflections);
    void apply_brush_entities(bool enabled, IPLVector3 listener);
    bool update_brush_instances(const std::vector<BrushEntityState>& states);
    bool select_near_brushes(IPLVector3 listener);
    const SubScene* sub_scene(int model_index);
    void release_brush_instances();
    void release_sub_scenes();
    IPLVector3 leave_own_brush(IPLVector3 source, int entnum, IPLVector3 listener) const;
    void update_sky(IPLVector3 listener, bool now);
    void place_voice(Voice& v, const VoiceParams& params, uint32_t generation, const EngineConfig& cfg, IPLVector3 listener);
    bool create_reverb(const EngineConfig& cfg, ReverbMode mode, const IPLAudioSettings& audio);

    // Steam Audio objects. The audio thread must not run. Raytracing has its own lock (sim_run_mutex_).
    struct HrtfInfo {
        std::string name = "default";
        std::string warning;
    };
    bool create_core(std::string& out_error);
    IPLHRTF create_hrtf(const EngineConfig& cfg, int rate, int frame, HrtfInfo& info) const;
    bool create_dsp(const EngineConfig& cfg, std::string& out_error, IPLHRTF hrtf = nullptr, const HrtfInfo* info = nullptr);
    void release_dsp();
    bool rebuild_dsp(const EngineConfig& cfg, int rate, int frame, std::string& out_error, IPLHRTF hrtf = nullptr,
                     const HrtfInfo* info = nullptr);
    void reset_audio_state();

    // Device thread (and init/shutdown, the headless caller).
    void device_thread_main();
    void device_loop(DefaultDeviceWatch& watch);
    void check_device(DefaultDeviceWatch& watch, bool reopen);
    void reopen_device(const char* reason);
    void apply_rebuild_request();
    bool open_device(const EngineConfig& cfg, int& out_rate, int& out_frame, std::string& out_error);
    bool start_device(std::string& out_error);
    void fade_out_and_wait();
    void close_device();
    bool device_running() const;
    void post_message(const std::string& text);

    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> stop_sim_{false};
    bool headless_ = false;
    std::atomic<bool> device_started_{false};

    mutable std::mutex cfg_mutex_; // never the audio thread
    EngineConfig cfg_{};

    // Settings the audio thread reads.
    std::atomic<float> master_volume_{1.0f};
    std::atomic<float> engine_volume_{1.0f};
    std::atomic<bool>  hrtf_on_{true};
    std::atomic<bool>  occlusion_on_{true};
    std::atomic<bool>  reverb_on_{true};
    std::atomic<float> reverb_wet_{0.55f};
    std::atomic<bool>  weather_occlusion_on_{true};
    std::atomic<float> weather_volume_{0.65f};
    std::atomic<float> rain_masking_{1.0f};
    std::atomic<bool>  glass_on_{true};
    std::atomic<bool>  tame_loud_{true};
    std::atomic<float> rain_level_{0.0f};   // published by the audio thread
    std::atomic<float> bass_cut_db_{0.0f};
    std::atomic<float> limiter_db_{0.0f};
    std::atomic<bool>  directivity_on_{true};
    std::atomic<bool>  mute_in_background_{true};
    std::atomic<bool>  focused_{true};
    std::atomic<bool>  game_paused_{false};
    std::atomic<bool>  held_{false};
    std::atomic<bool>  swap_mute_{false};   // the device thread is about to stop the callback
    std::atomic<bool>  gate_closed_{false}; // published by the audio thread: nothing is heard any more
    std::atomic<float> sky_exposure_{1.0f};
    SeqLock<GainModel>    gain_model_;
    SeqLock<ListenerPose> listener_pub_;
    SeqLock<PlayerHead>   player_pub_;
    SeqLock<IPLReflectionEffectParams> refl_pub_;
    std::atomic<bool> refl_valid_{false};

    // Fixed while the audio thread runs (create_dsp).
    int rate_ = kDryRate;
    int frame_ = kDefaultFrameSize;
    int32_t dry_per_block_ = kDefaultFrameSize; // most dry frames one block can consume
    int32_t min_fill_ = 0;      // dry frames a block needs so that it can still fade out
    int surplus_window_ = 0;    // blocks
    float gain_alpha_ = 1.0f, occ_alpha_ = 1.0f, weather_alpha_ = 1.0f, dir_alpha_ = 1.0f, glass_alpha_ = 1.0f;
    float rain_up_alpha_ = 1.0f, rain_down_alpha_ = 1.0f;
    float gate_step_ = 1.0f, room_down_step_ = 1.0f, room_up_step_ = 1.0f;
    int tail_blocks_ = 2, reverb_tail_ = 0;
    ReverbMode reverb_mode_ = ReverbMode::kParametric;
    int early_samples_ = 0;
    Resampler resampler_;

    // Steam Audio core objects.
    IPLContext    ipl_ctx_   = nullptr;
    IPLHRTF       ipl_hrtf_  = nullptr;
    IPLScene      ipl_scene_ = nullptr;
    IPLStaticMesh ipl_mesh_  = nullptr;
    IPLSimulator  ipl_sim_   = nullptr;
    IPLSource     reverb_source_ = nullptr; // sits at the listener: the reverb of the room the player is in

    // Shared reflection/reverb effects (1st-order Ambisonics = 4 channels, binaural decoded).
    static constexpr int kAmbisonicsOrder = 1;
    static constexpr int kAmbisonicsChannels = (kAmbisonicsOrder + 1) * (kAmbisonicsOrder + 1); // 4
    IPLReflectionEffect       refl_effect_ = nullptr;
    IPLAmbisonicsDecodeEffect ambi_decode_ = nullptr;

    Voice voices_[kMaxVoices];

    // Timeline: frames committed by the main thread / consumed by the audio thread.
    std::atomic<uint32_t> write_pos_{0};
    std::atomic<uint32_t> read_pos_{0};
    std::atomic<int32_t>  engine_ahead_{0};
    std::atomic<uint32_t> flush_epoch_{0};
    std::atomic<uint32_t> flush_to_{0};

    // Main thread only.
    Slot slots_[kMaxChannels];
    int  chunk_frames_ = 0;
    int  fed_voices_[kMaxVoices] = {};
    int  fed_count_ = 0;

    // Audio thread only.
    std::vector<float> scratch_dry_l_, scratch_dry_r_, scratch_mono_, scratch_direct_;
    std::vector<float> scratch_bin_l_, scratch_bin_r_, mix_l_, mix_r_, env_, fetch_;
    std::vector<float> rev_send_, rev_ambi_[kAmbisonicsChannels], rev_out_l_, rev_out_r_;
    std::vector<float> carry_;
    uint32_t carry_pos_ = 0;
    uint32_t carry_len_ = 0;
    IPLCoordinateSpace3 a_listener_{}; // newest published pose: what the block renders with
    GainModel a_gain_model_{};
    IPLReflectionEffectParams a_refl_{};
    bool     a_refl_known_ = false;
    bool     a_starved_ = true;
    bool     a_fade_in_ = false;
    uint32_t a_flush_epoch_ = 0;
    uint32_t a_skip_frames_ = 0;
    double   a_frac_ = 0.0;
    int32_t  a_max_surplus_ = 0;
    int      a_surplus_blocks_ = 0;
    int      a_rate_step_ = 0; // -1, 0, +1: clock drift correction (0.4 %)
    int      a_reverb_tail_blocks_ = 0;
    float    a_master_gain_ = -1.0f;
    float    a_gate_ = 1.0f;       // everything: 0 in the background
    float    a_room_ = 1.0f;       // the reverb: 0 while the game is paused
    bool     a_room_live_ = false; // the reverb effects may hold sound
    bool     a_held_ = false;
    float    a_rain_in_ = 0.0f;    // the loudest rain of this block
    float    a_rain_ = 0.0f;       // smoothed: what covers other sounds
    bool     a_raining_ = false;
    float    a_cover_ = 0.0f;      // a_rain_ times the strength the player chose
    int32_t  a_glass_entity_ = 0;  // who spoke through glass last, and how much: the next line starts like that
    float    a_glass_ = 0.0f;
    LoudnessLeveler a_bass_guard_;
    VoiceMemory a_voice_memory_;
    std::vector<float> listen_l_, listen_r_;
    PeakLimiter a_limiter_;

    // Raytracing thread wake-up and simulator ownership.
    std::mutex sim_run_mutex_;  // held while the simulator runs; scene changes take it too
    std::mutex sim_wake_mutex_;
    std::condition_variable sim_wake_;
    bool sim_kick_ = false;
    bool scene_dirty_ = false;  // under sim_run_mutex_
    mutable std::mutex scene_info_mutex_;
    uint32_t scene_tri_count_ = 0;
    uint32_t brush_model_count_ = 0;
    uint32_t glass_tri_count_ = 0;
    bool has_sky_ = false;
    std::string loaded_map_;
    std::thread sim_thread_;

    // Moving brushes. Main thread -> raytracing thread; everything else under sim_run_mutex_.
    mutable std::mutex brush_mutex_;
    std::vector<BrushEntityState> brush_states_;
    bool brush_dirty_ = false;
    std::vector<BrushEntityInfo> brush_infos_; // for sa_debug
    std::vector<BrushInstance> brush_instances_;
    std::vector<std::pair<float, std::size_t>> brush_order_;
    IPLVector3 brush_listener_{};
    std::vector<AcousticSubModel> brush_models_;
    std::vector<std::unique_ptr<SubScene>> sub_scenes_;
    std::vector<IPLMaterial> brush_materials_;
    SkyProbe sky_;
    std::vector<SkyProbe::Mover> sky_movers_;
    GlassProbe glass_;
    std::vector<GlassProbe::Mover> glass_movers_;
    uint32_t sky_countdown_ = 0;

    OutputPacer pacer_; // audio thread

    // Device thread only (after init).
    ma_device* ma_dev_ = nullptr;
    std::wstring device_id_;        // endpoint the open device belongs to
    std::wstring tolerated_id_;     // a default device that opening "the default device" does not lead to
    bool     open_failed_ = false;  // said so already
    bool     dsp_ready_ = false;
    bool     phonon_memory_ = false; // holds acquire_zeroed_phonon_memory
    uint32_t seen_callbacks_ = 0;
    uint64_t progress_ms_ = 0;      // when the callback last made progress
    uint64_t dead_after_ms_ = 0;    // without progress for so long: the device is dead (grows if opening it again does not help)

    // Device thread control.
    std::thread device_thread_;
    std::mutex ctl_mutex_;
    std::condition_variable ctl_cv_;
    bool ctl_stop_ = false;
    bool ctl_device_event_ = false;
    bool ctl_reopen_ = false;
    int  ctl_init_ = 0;             // 0: not done, 1: running, 2: failed
    std::string ctl_init_error_;
    bool ctl_want_mode_ = false, ctl_want_hrtf_ = false;
    ReverbMode   ctl_mode_ = ReverbMode::kHybrid;
    std::string  ctl_sofa_file_;
    std::wstring ctl_sofa_path_;
    std::atomic<bool> rebuild_pending_{false};

    std::mutex msg_mutex_;
    std::deque<std::string> messages_;
    std::atomic<bool> has_message_{false};

    mutable std::mutex status_mutex_;
    std::string device_name_;
    HrtfInfo hrtf_info_;

    // What get_stats() may read at any time.
    std::atomic<int> pub_rate_{kDryRate}, pub_frame_{kDefaultFrameSize}, pub_reverb_mode_{0};
    std::atomic<uint32_t> dev_rate_{0}, dev_period_{0}, dev_buffer_{0};
    std::atomic<uint32_t> callbacks_{0};
    std::atomic<uint32_t> device_changes_{0};
    std::atomic<uint32_t> reverb_faults_{0};
    std::atomic<uint32_t> dsp_rebuilds_{0};

    // Stats.
    std::atomic<uint64_t> blocks_rendered_{0};
    std::atomic<uint64_t> sim_ticks_{0};
    std::atomic<uint32_t> active_3d_count_{0};
    std::atomic<uint32_t> active_2d_count_{0};
    std::atomic<uint32_t> pose_age_us_{0};
    mutable std::atomic<uint32_t> render_us_max_{0};
    std::atomic<uint32_t> starvations_{0};
    std::atomic<uint32_t> dropped_chunks_{0};
    std::atomic<uint32_t> resyncs_{0};
    std::atomic<float>    reverb_time_[3] = {};
    std::atomic<uint32_t> queued_avg_{0};
    mutable std::atomic<uint32_t> queued_max_{0};
    std::atomic<uint32_t> reserve_{0};
    std::atomic<uint32_t> late_wakes_{0};
    std::atomic<uint32_t> brush_moves_{0};
    std::atomic<uint32_t> scene_commits_{0};
    std::atomic<uint32_t> sim_us_direct_{0};
    std::atomic<uint32_t> sim_us_reflections_{0};
};

} // namespace sa
