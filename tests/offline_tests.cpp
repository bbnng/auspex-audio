#include "bsp_acoustics.h"
#include "coord_util.h"
#include "channel_model.h"
#include "dynamics.h"
#include "lockfree.h"
#include "output_pacer.h"
#include "resampler.h"
#include "settings.h"
#include "sky_exposure.h"
#include "steam_audio_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::wstring g_sofa_file;
std::wstring g_maps_dir; // the game's own maps, read only

void check(bool cond, const char* msg) {
    ++g_checks;
    if (!cond) {
        std::printf("  [FAIL] %s\n", msg);
        ++g_failures;
    } else {
        std::printf("  [PASS] %s\n", msg);
    }
}

bool nearf(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

constexpr float kPi = 3.14159265f;

// Source basis of a camera at yaw `deg` (0 = +X, counter-clockwise like Source).
struct Camera {
    float org[3] = { 0, 0, 0 };
    float fwd[3] = { 1, 0, 0 };
    float rgt[3] = { 0, -1, 0 };
    float up[3]  = { 0, 0, 1 };
    void yaw(float deg) {
        const float r = deg * kPi / 180.0f;
        fwd[0] = std::cos(r); fwd[1] = std::sin(r);
        rgt[0] = std::sin(r); rgt[1] = -std::cos(r);
    }
};

void publish(sa::SteamAudioEngine& eng, const Camera& c) { eng.update_listener(c.org, c.fwd, c.rgt, c.up); }

sa::ChannelDescriptor source_at(float x, float y, float z, float dist_mult = 0.001f) {
    sa::ChannelDescriptor d{};
    d.active = true;
    d.spatial_3d = true;
    d.origin[0] = x; d.origin[1] = y; d.origin[2] = z;
    d.dist_mult = dist_mult;
    d.master_gain = 1.0f;
    d.mixer = 1;
    return d;
}

std::unique_ptr<sa::SteamAudioEngine> make_engine(bool reverb = false, sa::EngineConfig cfg = sa::EngineConfig()) {
    cfg.reverb_enabled = reverb;
    auto eng = std::make_unique<sa::SteamAudioEngine>();
    std::string err;
    const bool ok = eng->init(cfg, /*headless=*/true, err);
    if (!ok) std::printf("  init: %s\n", err.c_str());
    check(ok, "SteamAudioEngine headless init");
    return eng;
}

// A continuous test tone, so dropped or repeated frames show up as jumps.
struct Tone {
    float freq = 440.0f;
    float amp = 0.25f;
    double phase = 0.0;
    void next(float* out, int n) {
        for (int i = 0; i < n; ++i) {
            out[i] = amp * static_cast<float>(std::sin(phase));
            phase += 2.0 * 3.14159265358979 * freq / sa::kDryRate;
        }
    }
};

// Broadband, so left/right differences of the HRTF are large.
struct Noise {
    uint32_t s = 12345;
    void next(float* out, int n) {
        for (int i = 0; i < n; ++i) {
            s = s * 1664525u + 1013904223u;
            out[i] = 0.25f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
        }
    }
};

template <class Gen>
void feed(sa::SteamAudioEngine& eng, int ch, const sa::ChannelDescriptor& d, Gen& gen, int frames) {
    float buf[1020];
    while (frames > 0) {
        const int n = std::min(frames, 1020);
        gen.next(buf, n);
        eng.begin_chunk(1020);
        eng.feed_channel_mono(ch, d, buf, 0, n);
        eng.end_chunk(n);
        frames -= n;
    }
}

void feed_silence(sa::SteamAudioEngine& eng, int frames) {
    while (frames > 0) {
        const int n = std::min(frames, 1020);
        eng.begin_chunk(1020);
        eng.end_chunk(n);
        frames -= n;
    }
}

struct Energy { double l = 0, r = 0; };
Energy render(sa::SteamAudioEngine& eng, int frames, float* peak = nullptr) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    eng.render_output(out.data(), static_cast<uint32_t>(frames));
    Energy e;
    for (int i = 0; i < frames; ++i) {
        e.l += static_cast<double>(out[i * 2]) * out[i * 2];
        e.r += static_cast<double>(out[i * 2 + 1]) * out[i * 2 + 1];
        if (peak) *peak = std::max(*peak, std::max(std::fabs(out[i * 2]), std::fabs(out[i * 2 + 1])));
    }
    return e;
}

float dist_mult_for(float sound_level_db, float ref_dist, float ref_db = 60.0f) {
    return std::pow(10.0f, ref_db / 20.0f) / std::pow(10.0f, sound_level_db / 20.0f) / ref_dist;
}

// Reference transcription of engine.dll SND_GetGain (0x2011A0B0) in double precision.
double reference_gain(double dist, double dist_mult, const sa::GainModel& m) {
    const double rel = std::pow(10.0, dist / 1200.0 * m.foliage_db / 20.0) * dist * dist_mult;
    double gain = rel <= 0.1 ? m.snd_gain * 10.0 : 1.0 / rel * m.snd_gain;
    if (gain > 0.5) {
        double ex = 2.5;
        const int lvl = static_cast<int>(std::log10(std::pow(10.0, m.ref_db / 20.0) / (m.ref_dist * dist_mult)) * 20.0);
        if (lvl > 90) ex = 2.5 - (lvl - 90.0) * 1.7 / 50.0;
        const double k = -1.0 / (std::pow(0.5, ex) * -0.5);
        gain = m.gain_max * (1.0 - 1.0 / (std::pow(gain, ex) * k));
    }
    if (gain < m.gain_min) gain = (2.0 - m.gain_min * rel) * m.gain_min;
    return gain < 0.0 ? 0.0 : gain;
}

void test_coords() {
    std::printf("=== Test 1: Coordinates, SND_GetGain proximity, near field ===\n");
    const IPLVector3 fwd_pos = sa::source_pos_to_ipl(100.0f, 0.0f, 0.0f);
    check(nearf(fwd_pos.x, 0.0f) && nearf(fwd_pos.y, 0.0f) && nearf(fwd_pos.z, -2.54f),
          "Source +X (100 in) -> Steam Audio (0, 0, -2.54 m)");
    const IPLVector3 right_pos = sa::source_pos_to_ipl(0.0f, -100.0f, 0.0f);
    check(nearf(right_pos.x, 2.54f) && nearf(right_pos.y, 0.0f) && nearf(right_pos.z, 0.0f),
          "Source -Y (100 in) -> Steam Audio (+2.54 m, 0, 0)");

    Camera cam;
    const IPLCoordinateSpace3 cs = sa::make_ipl_listener(cam.org, cam.fwd, cam.rgt, cam.up);
    float dist = 0.0f;
    const IPLVector3 local = sa::world_dir_to_listener_local(cs, right_pos, &dist);
    check(nearf(local.x, 1.0f) && nearf(local.y, 0.0f) && nearf(local.z, 0.0f) && nearf(dist, 2.54f),
          "world_dir_to_listener_local maps Source right to +X");

    // The proximity curve is the engine's own.
    sa::GainModel up_patch;
    up_patch.ref_dist = 50.0f; // Unofficial Patch autoexec.cfg
    const sa::GainModel models[] = { sa::GainModel(), up_patch };
    float worst = 0.0f, worst_step = 0.0f;
    bool monotonic = true;
    for (const sa::GainModel& m : models) {
        for (float level = 50.0f; level <= 140.0f; level += 7.5f) {
            const float dm = dist_mult_for(level, m.ref_dist);
            float prev = -1.0f;
            for (float d = 0.0f; d < 20000.0f; d += (d < 400.0f ? 1.0f : 25.0f)) {
                const float g = sa::compute_source_distance_attenuation(d * sa::kSourceToMeters, dm, m);
                worst = std::max(worst, std::fabs(g - static_cast<float>(reference_gain(d, dm, m))));
                if (prev >= 0.0f) {
                    if (g > prev + 1e-5f) monotonic = false;
                    if (d < 400.0f) worst_step = std::max(worst_step, prev - g);
                }
                prev = g;
            }
        }
    }
    std::printf("    worst deviation from SND_GetGain %.5f, largest step per inch %.4f\n", worst, worst_step);
    check(worst < 2e-3f, "Attenuation equals engine.dll SND_GetGain for 50..140 dB, snd_refdist 36 and 50");
    check(monotonic, "Attenuation never rises with distance (no spikes)");
    check(worst_step < 0.06f, "Attenuation never jumps while walking (per inch)");

    // A fridge hum (60 dB, snd_refdist 50): clearly audible across a kitchen, not only with the ear on the door.
    const float fridge = dist_mult_for(60.0f, 50.0f);
    const float g_touch = sa::compute_source_distance_attenuation(0.3f, fridge, up_patch);
    const float g_1m = sa::compute_source_distance_attenuation(1.0f, fridge, up_patch);
    const float g_3m = sa::compute_source_distance_attenuation(3.0f, fridge, up_patch);
    const float g_6m = sa::compute_source_distance_attenuation(6.0f, fridge, up_patch);
    std::printf("    60 dB fridge: 0.3 m %.2f | 1 m %.2f | 3 m %.2f | 6 m %.2f\n", g_touch, g_1m, g_3m, g_6m);
    check(g_touch > 0.9f && g_1m > 0.75f && g_3m > 0.3f && g_6m > 0.12f, "Fridge hum stays audible at room distances");
    check(nearf(sa::compute_source_distance_attenuation(5.0f, 0.0f), 1.0f), "Unattenuated channels play at full level");

    check(nearf(sa::compute_spatial_blend(2.0f, 0.0f), 1.0f) && nearf(sa::compute_spatial_blend(0.0f, 0.0f), 0.0f),
          "Sources localize fully from 0.6 m on and fade to centred inside the head");
    check(sa::compute_spatial_blend(100.0f * sa::kSourceToMeters, 200.0f) < 0.01f &&
          sa::compute_spatial_blend(200.0f * sa::kSourceToMeters, 200.0f) > 0.99f, "channel radius widens the near field");
}

void test_lockfree() {
    std::printf("=== Test 2: Lock-free pose exchange and dry ring ===\n");
    struct Payload { uint32_t v[13]; };
    sa::SeqLock<Payload> lock;
    Payload p{};
    check(!lock.load(p), "SeqLock reports that nothing was published yet");

    std::atomic<bool> stop{ false };
    std::thread writer([&] {
        for (uint32_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            Payload w;
            for (uint32_t i = 0; i < 13; ++i) w.v[i] = n * 31u + i;
            lock.store(w);
        }
    });
    uint64_t reads = 0, torn = 0, missed = 0, stale = 0;
    uint32_t last = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        Payload r{};
        if (!lock.load(r)) { ++missed; continue; }
        ++reads;
        for (uint32_t i = 1; i < 13; ++i) torn += (r.v[i] != r.v[0] + i) ? 1 : 0;
        if (static_cast<int32_t>(r.v[0] - last) < 0) ++stale;
        last = r.v[0];
    }
    stop = true;
    writer.join();
    std::printf("    %llu reads, %llu torn, %llu out of order, %llu given up\n",
                static_cast<unsigned long long>(reads), static_cast<unsigned long long>(torn),
                static_cast<unsigned long long>(stale), static_cast<unsigned long long>(missed));
    check(reads > 1000 && torn == 0 && stale == 0, "SeqLock never hands the audio thread a torn or older pose");

    sa::DryRing ring;
    check(ring.ensure_allocated(), "DryRing allocates");
    std::vector<float> in(600), out(600);
    for (int i = 0; i < 600; ++i) in[i] = static_cast<float>(i);
    const uint32_t pos = 0xFFFFFFFFu - 250u; // wraps the 32-bit timeline and the ring at once
    ring.write(pos, in.data(), 600);
    ring.read(pos, out.data(), 600);
    check(in == out, "DryRing keeps data across the timeline and ring wrap");
    ring.zero(pos + 100, 50);
    ring.read(pos, out.data(), 600);
    check(out[99] == 99.0f && out[100] == 0.0f && out[149] == 0.0f && out[150] == 150.0f, "DryRing zeroes exactly the gap");
    check(sa::frame_diff(5u, 0xFFFFFFFBu) == 10, "Timeline distance is right across the wrap");
}

void test_bsp_clipping_and_materials() {
    std::printf("=== Test 3: BSP brush clipping, materials, surfaces ===\n");
    check(sa::classify_surface_material("CONCRETE/SEWER_WALL01") == sa::kMatConcrete, "Concrete surface classified");
    check(sa::classify_surface_material("WOOD/ASYLUM_DOOR") == sa::kMatWood, "Wood/door surface classified");
    check(sa::classify_surface_material("GLASS/WINDOW_03") == sa::kMatGlass, "Glass surface classified");
    check(sa::classify_surface_material("METAL/VENT_GRATE") == sa::kMatMetal, "Metal surface classified");
    check(sa::classify_surface_material("CARPET/HOTEL_HALL") == sa::kMatCarpet, "Carpet surface classified");
    check(sa::classify_surface_material("maps/la_ventruetower_2/plaster/twroffwllc_-1819_204_3871") == sa::kMatDefault,
          "A baked plaster wall in la_VENTruetower is not a metal vent");
    check(sa::classify_surface_material("maps/la_malkavian_2/tile/malkfloorb") == sa::kMatTile, "Baked tile floor classified");
    check(sa::classify_surface_material("maps/sm_shreknet_1/metal/vente") == sa::kMatMetal, "Baked vent classified");
    check(sa::classify_surface_material("plaster/building2door") == sa::kMatWood, "Door on a plaster texture is wood");
    check(sa::classify_surface_material("stone/giowllg") == sa::kMatConcrete, "Stone classified");
    check(sa::classify_surface_material("drapery/curtain_red") == sa::kMatCarpet, "Curtains absorb");
    check(sa::classify_surface_material("models/environmental/thing") == sa::kMatDefault, "'envIRONment' is not iron");
    check(sa::classify_surface_material("") == sa::kMatDefault && sa::classify_surface_material(nullptr) == sa::kMatDefault,
          "Empty names are safe");

    check(sa::surface_lets_sound_through("tools/toolsskybox", 0x404), "Sky lets sound out");
    check(sa::surface_lets_sound_through("nature/sky", 0x4), "SURF_SKY lets sound out whatever the texture");
    check(sa::surface_lets_sound_through("TOOLS/TOOLSTRIGGER", 0x400) && sa::surface_lets_sound_through("tools/toolsclip", 0) &&
          sa::surface_lets_sound_through("tools/toolsinvisible", 0) && sa::surface_lets_sound_through("tools/toolshint", 0),
          "Triggers, clips, invisible and hint brushes are no obstacle");
    check(!sa::surface_lets_sound_through("tools/toolsnodraw", 0x480) && !sa::surface_lets_sound_through("tools/toolsblack", 0) &&
          !sa::surface_lets_sound_through("metal/shadowbox", 0), "Nodraw backs of walls and real textures stay solid");

    const sa::AcousticBrushPlane wall[6] = {
        {  1,  0,  0,  110.0f, sa::kMatConcrete },
        { -1,  0,  0,  -90.0f, sa::kMatConcrete },
        {  0,  1,  0,  200.0f, sa::kMatConcrete },
        {  0, -1,  0,  200.0f, sa::kMatConcrete },
        {  0,  0,  1,  200.0f, sa::kMatConcrete },
        {  0,  0, -1,  200.0f, sa::kMatConcrete },
    };
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "test_wall";
    sa::clip_brush_to_acoustic_mesh(wall, 6, mesh);
    check(mesh.triangles.size() == 12, "Axis-aligned box brush clips to exactly 12 triangles");
    check(mesh.material_indices.size() == 12 && mesh.material_indices[0] == sa::kMatConcrete,
          "Material indices match clipped faces");
}

void add_box(sa::AcousticMeshData& mesh, float x0, float x1, float y0, float y1, float z0, float z1) {
    const sa::AcousticBrushPlane box[6] = {
        {  1, 0, 0, x1, sa::kMatConcrete }, { -1, 0, 0, -x0, sa::kMatConcrete },
        {  0, 1, 0, y1, sa::kMatConcrete }, { 0, -1, 0, -y0, sa::kMatConcrete },
        {  0, 0, 1, z1, sa::kMatConcrete }, { 0, 0, -1, -z0, sa::kMatConcrete },
    };
    sa::clip_brush_to_acoustic_mesh(box, 6, mesh);
}

sa::AcousticSubModel box_model(int index, float x0, float x1, float y0, float y1, float z0, float z1) {
    const sa::AcousticBrushPlane box[6] = {
        {  1, 0, 0, x1, sa::kMatWood }, { -1, 0, 0, -x0, sa::kMatWood },
        {  0, 1, 0, y1, sa::kMatWood }, { 0, -1, 0, -y0, sa::kMatWood },
        {  0, 0, 1, z1, sa::kMatWood }, { 0, 0, -1, -z0, sa::kMatWood },
    };
    sa::AcousticSubModel m;
    m.model_index = index;
    sa::clip_brush_to_acoustic_mesh(box, 6, m);
    return m;
}

// A room around everything within +-400.
void add_room(sa::AcousticMeshData& mesh) {
    add_box(mesh, -420, 420, -420, -400, -320, 320);
    add_box(mesh, -420, 420, 400, 420, -320, 320);
    add_box(mesh, 400, 420, -420, 420, -320, 320);
    add_box(mesh, -420, -400, -420, 420, -320, 320);
    add_box(mesh, -420, 420, -420, 420, 300, 320);
    add_box(mesh, -420, 420, -420, 420, -320, -300);
}

void test_steam_audio_live_pipeline() {
    std::printf("=== Test 4: Live Steam Audio (phonon.dll) HRTF + occlusion + reverb ===\n");
    auto eng = make_engine(/*reverb=*/true);
    if (!eng->is_initialized()) return;

    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "test_room";
    add_box(mesh, 90, 110, -300, 300, -300, 300);    // the wall between listener and channel 0
    add_box(mesh, -420, 420, -420, -400, -320, 320); // a room around everything
    add_box(mesh, -420, 420, 400, 420, -320, 320);
    add_box(mesh, 400, 420, -420, 420, -320, 320);
    add_box(mesh, -420, -400, -420, 420, -320, 320);
    add_box(mesh, -420, 420, -420, 420, 300, 320);
    add_box(mesh, -420, 420, -420, 420, -320, -300);
    std::string summary;
    check(eng->load_scene(mesh, summary), ("load_scene: " + summary).c_str());

    Camera cam;
    publish(*eng, cam);
    const sa::ChannelDescriptor occluded = source_at(200.0f, 0.0f, 0.0f);
    const sa::ChannelDescriptor open = source_at(0.0f, -200.0f, 0.0f);
    Noise n0, n1;
    float buf[512];
    eng->begin_chunk(512);
    n0.next(buf, 512);
    eng->feed_channel_mono(0, occluded, buf, 0, 512);
    n1.next(buf, 512);
    eng->feed_channel_mono(1, open, buf, 0, 512);
    eng->end_chunk(512);
    eng->run_simulation_sync();

    IPLDirectEffectParams p_occ{}, p_open{};
    check(eng->get_channel_direct_params(0, p_occ), "Got direct simulation params for occluded channel");
    check(eng->get_channel_direct_params(1, p_open), "Got direct simulation params for open channel");
    std::printf("    occluded: occ=%.3f trans=(%.3f, %.3f, %.3f) | open: occ=%.3f\n",
                p_occ.occlusion, p_occ.transmission[0], p_occ.transmission[1], p_occ.transmission[2], p_open.occlusion);
    check(p_occ.occlusion < 0.25f && p_open.occlusion > 0.90f,
          "Concrete wall strongly occludes source behind it while open source stays unoccluded");
    check(p_occ.transmission[0] > p_occ.transmission[2],
          "Transmission passes more bass than treble through the wall");
    const sa::EngineStats st = eng->get_stats();
    std::printf("    simulated RT60: %.2f / %.2f / %.2f s\n", st.reverb_time[0], st.reverb_time[1], st.reverb_time[2]);
    check(st.reverb_time[0] > 0.1f && st.reverb_time[0] < 10.0f, "Listener-centred reverb has a plausible decay time");

    // Right-hand source: stronger right ear.
    eng->notify_channel_freed(0);
    feed(*eng, 1, open, n1, 2048);
    render(*eng, 1024);
    const Energy e = render(*eng, 1024);
    std::printf("    right-side source HRTF energy: L=%.5f R=%.5f\n", e.l, e.r);
    check(e.r > e.l * 1.5 && e.r > 0.01, "HRTF gives the right ear more energy for a right-side source");

    // Reverb rings on after the source stopped.
    eng->notify_channel_freed(1);
    feed_silence(*eng, 4096);
    render(*eng, 1024);
    const Energy tail = render(*eng, 1024);
    std::printf("    reverb tail energy after the source ended: %.6f\n", tail.l + tail.r);
    check(tail.l + tail.r > 1e-7, "Reverb tail rings on in the raytraced room");
    eng->unload_scene();
    eng->shutdown();
}

void test_camera_turn_latency() {
    std::printf("=== Test 5: Camera turn latency with snd_mixahead 0.2 ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    const sa::ChannelDescriptor fridge = source_at(0.0f, -60.0f, 0.0f, dist_mult_for(60, 50)); // to the right
    Noise noise;
    feed(*eng, 5, fridge, noise, 8820); // what VTMB paints ahead: 200 ms of fridge, all queued before the turn
    render(*eng, 2048);
    const Energy before = render(*eng, 512);
    check(before.r > before.l * 2.0, "Fridge on the right before the turn");

    const uint32_t queued = eng->buffered_frames();
    cam.yaw(-90.0f);  // turn right: the fridge is now straight ahead
    publish(*eng, cam);
    const int block = eng->frame_size();
    const Energy first = render(*eng, block); // cross-fades from the old to the new direction
    const Energy second = render(*eng, block);
    const Energy third = render(*eng, block);
    const Energy settled = render(*eng, 2048 - 3 * block);
    const double first_db = 10.0 * std::log10(first.r / first.l);
    const double second_db = 10.0 * std::log10((second.r + third.r) / (second.l + third.l));
    const double settled_db = 10.0 * std::log10(settled.r / settled.l);
    std::printf("    right-left: before %.1f dB | after a 90 degree turn: first 2.9 ms %.1f dB, 2.9-8.7 ms %.1f dB | settled in"
                " front %.1f dB (queued dry audio: %.0f ms)\n",
                10.0 * std::log10(before.r / before.l), first_db, second_db, settled_db, queued * 1000.0 / sa::kDryRate);
    check(std::fabs(settled_db) < 3.0, "Settled direction is in front");
    check(first_db < 10.0 * std::log10(before.r / before.l) - 2.0, "The very first block after the turn already moves");
    check(std::fabs(second_db - settled_db) < 2.5, "From the second block (2.9 ms) after the turn on the fridge is in front");

    cam.yaw(-170.0f); // keep turning: now it is on the left
    publish(*eng, cam);
    render(*eng, 512);
    const Energy left = render(*eng, 256);
    check(left.l > left.r * 1.5, "Fridge crosses to the left ear just as fast");
    check(queued - eng->buffered_frames() == 2048u + 512u + 256u, "Not a single dry frame was dropped to get there");
    check(eng->get_stats().starvations == 0 && eng->get_stats().resyncs == 0, "No stalls, no skips");

    // A smooth 180 deg/s turn at 60 fps must not click: compare sample steps against a still camera.
    auto turning = make_engine();
    Camera t;
    publish(*turning, t);
    Tone tone;
    tone.freq = 300.0f;
    const sa::ChannelDescriptor src = source_at(0.0f, -60.0f, 0.0f);
    float max_step = 0.0f, prev_l = 0.0f, prev_r = 0.0f;
    std::vector<float> out(735 * 2);
    feed(*turning, 1, src, tone, 4410);
    for (int frame = 0; frame < 60; ++frame) {
        feed(*turning, 1, src, tone, 735);
        t.yaw(-3.0f * static_cast<float>(frame));
        publish(*turning, t);
        turning->render_output(out.data(), 735);
        for (int i = 0; i < 735; ++i) {
            if (frame > 3) {
                max_step = std::max(max_step, std::fabs(out[i * 2] - prev_l));
                max_step = std::max(max_step, std::fabs(out[i * 2 + 1] - prev_r));
            }
            prev_l = out[i * 2];
            prev_r = out[i * 2 + 1];
        }
    }
    const float clean_step = 0.25f * 2.0f * kPi * 300.0f / 44100.0f; // steepest slope of the tone itself
    std::printf("    largest sample step while turning %.4f (tone alone %.4f)\n", max_step, clean_step);
    check(max_step < clean_step * 4.0f, "Turning the camera is click-free");
}

void test_source_moves_live() {
    std::printf("=== Test 6: Proximity and moving sources apply to queued audio ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    sa::ChannelDescriptor d = source_at(400.0f, 0.0f, 0.0f, dist_mult_for(60, 50));
    Noise noise;
    feed(*eng, 3, d, noise, 13230);
    render(*eng, 2048);
    const Energy far_e = render(*eng, 1024);
    d.origin[0] = 60.0f;
    eng->update_channel(3, d); // S_Update published a new origin; nothing new was painted
    render(*eng, 1024);        // moved more than 3 m: snaps, gain follows in ~10 ms
    const Energy near_e = render(*eng, 1024);
    std::printf("    energy 10 m -> 1.5 m: %.5f -> %.5f\n", far_e.l + far_e.r, near_e.l + near_e.r);
    check(near_e.l + near_e.r > (far_e.l + far_e.r) * 8.0, "Getting close is heard at once, not snd_mixahead later");

    // A stale descriptor (another sound in the slot) must not move this voice.
    sa::ChannelDescriptor other = d;
    other.mixer = 77;
    other.origin[0] = 4000.0f;
    eng->update_channel(3, other);
    const Energy same = render(*eng, 1024);
    check(same.l + same.r > (near_e.l + near_e.r) * 0.5, "Another sound's update in the same slot is ignored");

    // Walk from 8 m to 0.3 m in 2 s: the level only rises, in small steps, without clicks.
    auto walk = make_engine();
    publish(*walk, cam);
    Tone tone;
    const float dm = dist_mult_for(60, 50);
    sa::ChannelDescriptor w = source_at(315.0f, 0.0f, 0.0f, dm);
    feed(*walk, 1, w, tone, 4410);
    double prev = 0.0, worst_db = 0.0;
    bool rising = true;
    for (int f = 0; f < 120; ++f) {
        w.origin[0] = 315.0f - 303.0f * static_cast<float>(f) / 119.0f;
        feed(*walk, 1, w, tone, 735);
        walk->update_channel(1, w);
        const Energy e = render(*walk, 735);
        const double rms = std::sqrt((e.l + e.r) / 1470.0);
        if (f > 4 && w.origin[0] > 40.0f) {
            const double db = 20.0 * std::log10(rms / prev);
            worst_db = std::max(worst_db, std::fabs(db));
            if (db < -0.5) rising = false;
        }
        prev = rms;
    }
    std::printf("    largest level change per rendered frame while walking: %.2f dB\n", worst_db);
    check(rising && worst_db < 1.5, "Walking up to a source gets louder gradually");
}

void test_loop_cull_fade() {
    std::printf("=== Test 7: Looping sounds fade out where the engine stops mixing them ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    Tone tone;
    const float dm = dist_mult_for(60, 50);
    double prev = -1.0;
    float at_cull = 1.0f;
    bool smooth = true;
    sa::ChannelDescriptor d = source_at(300.0f, 0.0f, 0.0f, dm);
    d.loop_master = 1.0f;
    feed(*eng, 2, d, tone, 4410);
    for (float x = 300.0f; x < 1500.0f; x += 10.0f) {
        d.origin[0] = x;
        feed(*eng, 2, d, tone, 735);
        eng->update_channel(2, d);
        const Energy e = render(*eng, 735);
        const double rms = std::sqrt((e.l + e.r) / 1470.0);
        if (prev >= 0.0 && x > 350.0f && std::fabs(rms - prev) > 0.004) smooth = false;
        prev = rms;
        const float level = sa::compute_source_distance_attenuation(x * sa::kSourceToMeters, dm);
        if (level < sa::kCullLevel) { at_cull = static_cast<float>(rms); break; }
    }
    std::printf("    level where MIX_MixChannels culls the loop: %.6f\n", at_cull);
    check(smooth && at_cull < 1e-3f, "A loop is already silent when the engine stops mixing it (no pop)");

    // The engine skips it, then mixes it again: fades in, no click.
    feed_silence(*eng, 9000);
    render(*eng, 13000);
    d.origin[0] = 60.0f;
    feed(*eng, 2, d, tone, 4410);
    std::vector<float> out(4000 * 2);
    eng->render_output(out.data(), 4000);
    float max_step = 0.0f, peak = 0.0f;
    for (int i = 1; i < 4000; ++i) {
        max_step = std::max(max_step, std::fabs(out[i * 2] - out[(i - 1) * 2]));
        peak = std::max(peak, std::fabs(out[i * 2]));
    }
    check(peak > 0.05f && max_step < 0.06f, "A loop that comes back fades in without a click");
}

void test_voice_lifecycle() {
    std::printf("=== Test 8: Sounds play to their end at their own place ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    Noise a, b;
    sa::ChannelDescriptor right = source_at(0.0f, -80.0f, 0.0f);
    sa::ChannelDescriptor left = source_at(0.0f, 80.0f, 0.0f);
    left.mixer = 2;
    feed(*eng, 9, right, a, 4080);
    eng->notify_channel_freed(9);     // the engine mixed the last sample: 4080 frames are still to be heard
    check(!eng->channel_has_voice(9), "The slot is free for the next sound at once");
    feed(*eng, 9, left, b, 4080);     // another sound takes the slot
    check(eng->voices_in_use() == 2, "The old sound keeps its own voice");
    render(*eng, 1024);
    const Energy first = render(*eng, 2048);
    check(first.r > first.l * 2.0, "The finished sound still comes from its own side");
    render(*eng, 1536);
    const Energy second = render(*eng, 2048);
    check(second.l > second.r * 2.0, "The new sound comes from the new side");
    feed_silence(*eng, 4096);
    render(*eng, 3000);
    check(eng->voices_in_use() == 1, "The finished voice was recycled");

    // Slot reuse that the engine does not announce (MIX_MixChannels frees channels inline): told apart by mixer.
    sa::ChannelDescriptor again = right;
    again.mixer = 3;
    feed(*eng, 9, again, a, 1020);
    check(eng->voices_in_use() == 2, "A new mixer in the same slot is a new sound");

    // Never runs out: thousands of short sounds through few slots.
    Tone tone;
    for (int i = 0; i < 3000; ++i) {
        sa::ChannelDescriptor d = source_at(50.0f, 0.0f, 0.0f);
        d.mixer = 100u + static_cast<unsigned>(i);
        feed(*eng, i % 4, d, tone, 64);
        if (i % 3 == 0) eng->notify_channel_freed(i % 4);
        if (eng->buffered_frames() > 2048) render(*eng, 1024);
    }
    for (int i = 0; i < sa::kMaxChannels; ++i) eng->notify_channel_freed(i);
    feed_silence(*eng, 8192);
    render(*eng, 11000);
    check(eng->voices_in_use() == 0, "No voice leaks after 3000 sounds");
}

void test_stream_integrity() {
    std::printf("=== Test 9: 2D stream is exact, across chunks and odd callback sizes ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    sa::ChannelDescriptor music{};
    music.active = true;
    music.reverb_gain = 0.0f;
    music.mixer = 1;
    Tone left_tone, right_tone;
    right_tone.freq = 587.0f;
    Tone left_ref = left_tone, right_ref = right_tone;
    const int sizes[] = { 441, 512, 6, 441, 128, 1, 735, 259 };
    float worst = 0.0f;
    int rendered = 0;
    float l[1020], r[1020], ol[1024], orr[1024], out[2048];
    for (int round = 0; round < 400; ++round) {
        while (eng->buffered_frames() < 3000) {
            const int n = (round % 2) ? 1020 : 380;
            left_tone.next(l, n);
            right_tone.next(r, n);
            eng->begin_chunk(1020);
            // Looping sounds arrive in several pieces per chunk.
            eng->feed_channel_stereo(0, music, l, r, 0, 100);
            eng->feed_channel_stereo(0, music, l + 100, r + 100, 100, n - 100);
            eng->end_chunk(n);
        }
        const int n = sizes[round % 8];
        eng->render_output(out, static_cast<uint32_t>(n));
        left_ref.next(ol, n);
        right_ref.next(orr, n);
        for (int i = 0; i < n; ++i, ++rendered) {
            if (rendered < 1024) continue; // start-up fade
            worst = std::max(worst, std::fabs(out[i * 2] - sa::kLevel2D * ol[i]));
            worst = std::max(worst, std::fabs(out[i * 2 + 1] - sa::kLevel2D * orr[i]));
        }
    }
    std::printf("    %d frames, largest deviation %.6f\n", rendered, worst);
    check(worst < 1e-4f, "Every sample comes out once, in order, unchanged");

    // Trimmed chunks: what TransferSamples did not take is taken back.
    auto trim = make_engine();
    publish(*trim, cam);
    Tone t, ref;
    worst = 0.0f;
    for (int i = 0; i < 1020; ++i) l[i] = 0.9f; // garbage past the real chunk end
    for (int k = 0; k < 20; ++k) {
        t.next(l, 200);
        trim->begin_chunk(1020);
        trim->feed_channel_mono(0, music, l, 0, 1020);
        trim->end_chunk(200);
    }
    trim->render_output(out, 1024);
    trim->render_output(out, 1024);
    ref.next(ol, 1024);
    ref.next(ol, 1024);
    for (int i = 0; i < 1024; ++i) worst = std::max(worst, std::fabs(out[i * 2] - sa::kLevel2D * ol[i]));
    check(worst < 1e-4f, "Chunks longer than the engine really painted are trimmed cleanly");

    // Discarded catch-up chunks leave no trace.
    trim->begin_chunk(1020);
    trim->feed_channel_mono(0, music, l, 0, 1020);
    const uint32_t fill = trim->buffered_frames();
    trim->discard_chunk();
    check(trim->buffered_frames() == fill, "Discarded chunk does not enter the timeline");
    t.next(l, 1020);
    trim->begin_chunk(1020);
    trim->feed_channel_mono(0, music, l, 0, 1020);
    trim->end_chunk(1020);
    trim->render_output(out, 1024);
    trim->render_output(out, 1024);
    float max_step = 0.0f;
    for (int i = 1; i < 1024; ++i) max_step = std::max(max_step, std::fabs(out[i * 2] - out[i * 2 - 2]));
    check(max_step < 0.03f, "The stream continues seamlessly after it");
}

void test_hitches() {
    std::printf("=== Test 10: Hitches, backlog, clock drift ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    sa::ChannelDescriptor music{};
    music.active = true;
    music.mixer = 1;
    Tone tone;
    const int ahead = 8820;
    eng->set_engine_ahead(ahead);
    feed(*eng, 0, music, tone, ahead);
    float out[735 * 2];
    float max_step = 0.0f, prev = 0.0f;
    auto play = [&](int frames) {
        while (frames > 0) {
            const int n = std::min(frames, 735);
            eng->render_output(out, static_cast<uint32_t>(n));
            for (int i = 0; i < n; ++i) {
                max_step = std::max(max_step, std::fabs(out[i * 2] - prev));
                prev = out[i * 2];
            }
            frames -= n;
        }
    };
    for (int f = 0; f < 60; ++f) { feed(*eng, 0, music, tone, 735); play(735); }
    check(std::abs(static_cast<int>(eng->buffered_frames()) - ahead) < 1024, "Steady state: queue equals the engine's mix-ahead");
    check(eng->get_stats().starvations == 0, "60 fps never starves");

    // A one second hitch: plays out, fades, stays silent, then rejoins the engine's clock without extra delay.
    play(44100);
    check(eng->get_stats().starvations == 1, "A long hitch starves exactly once");
    feed(*eng, 0, music, tone, 1020 * 25); // the engine paints the missed second and the new mix-ahead
    play(735);
    const int after = static_cast<int>(eng->buffered_frames());
    std::printf("    queued after the hitch: %d frames (mix-ahead %d)\n", after, ahead);
    check(std::abs(after - ahead) < 2048, "Backlog of a hitch does not become permanent delay");
    for (int f = 0; f < 30; ++f) { feed(*eng, 0, music, tone, 735); play(735); }
    const float tone_step = 0.25f * 2.0f * kPi * 440.0f / 44100.0f;
    std::printf("    largest sample step through all of it %.4f (tone alone %.4f)\n", max_step, tone_step);
    check(max_step < tone_step * 2.0f, "Stall and recovery are click-free");

    // Surplus without a stall: jumps back with a cross-fade.
    max_step = 0.0f;
    feed(*eng, 0, music, tone, 8820);
    for (int f = 0; f < 120; ++f) { feed(*eng, 0, music, tone, 735); play(735); }
    check(std::abs(static_cast<int>(eng->buffered_frames()) - ahead) < 2048, "A large surplus is dropped once");
    check(max_step < tone_step * 2.0f, "Dropping it is click-free");

    // 0.2 % clock drift both ways for 60 s each: held by 0.4 % rate steps, no jumps.
    const uint32_t resyncs = eng->get_stats().resyncs;
    max_step = 0.0f;
    int lo = 1 << 30, hi = 0;
    for (int dir = -1; dir <= 1; dir += 2) {
        double acc = 0.0;
        for (int f = 0; f < 3600; ++f) {
            acc += 735.0 * (1.0 + dir * 0.002);
            const int n = static_cast<int>(acc);
            acc -= n;
            feed(*eng, 0, music, tone, n);
            play(735);
            lo = std::min(lo, static_cast<int>(eng->buffered_frames()));
            hi = std::max(hi, static_cast<int>(eng->buffered_frames()));
        }
    }
    std::printf("    0.2%% drift: queue stayed within %d..%d frames\n", lo, hi);
    check(lo > ahead - 4096 && hi < ahead + 4096, "Clock drift stays bounded");
    check(eng->get_stats().resyncs == resyncs && eng->get_stats().starvations == 1, "... without skips or stalls");
    check(max_step < tone_step * 1.5f, "... and without clicks");

    // Device gone: chunks are dropped, nothing is overwritten, recovery works.
    feed(*eng, 0, music, tone, 60000);
    check(eng->get_stats().dropped_chunks > 0 && eng->buffered_frames() <= sa::DryRing::kCapacity, "A dead output device cannot overrun the ring");
    for (int f = 0; f < 120; ++f) { feed(*eng, 0, music, tone, 735); play(735); }
    check(std::abs(static_cast<int>(eng->buffered_frames()) - ahead) < 2048, "Playback realigns when the device comes back");
}

void test_flush_and_toggle() {
    std::printf("=== Test 11: Flush and sa_enable A/B ===\n");
    auto eng = make_engine();
    Camera cam;
    publish(*eng, cam);
    Tone tone;
    const sa::ChannelDescriptor d = source_at(60.0f, 0.0f, 0.0f);
    feed(*eng, 1, d, tone, 8820);
    render(*eng, 2048);
    eng->flush_audio();
    std::vector<float> out(1024 * 2);
    eng->render_output(out.data(), 1024);
    float max_step = 0.0f, late = 0.0f;
    for (int i = 1; i < 1024; ++i) {
        max_step = std::max(max_step, std::fabs(out[i * 2] - out[i * 2 - 2]));
        if (i > 600) late = std::max(late, std::fabs(out[i * 2]));
    }
    check(late < 1e-5f && max_step < 0.06f, "stopsound / level change silences within 12 ms, without a click");
    render(*eng, 2048);
    check(eng->voices_in_use() == 0 && eng->buffered_frames() == 0, "Everything queued is gone");

    feed(*eng, 1, d, tone, 4410);
    render(*eng, 1024);
    eng->set_enabled(false);
    render(*eng, 1024);
    float peak = 0.0f;
    render(*eng, 2048, &peak);
    check(peak < 1e-5f, "sa_enable 0 is silent (vanilla plays)");
    eng->set_enabled(true);
    peak = 0.0f;
    render(*eng, 1024, &peak);
    check(peak < 1e-5f, "sa_enable 1 does not replay stale audio");
    feed(*eng, 1, d, tone, 4410);
    render(*eng, 1024);
    peak = 0.0f;
    render(*eng, 1024, &peak);
    check(peak > 0.02f, "... and plays what the engine mixes from then on");

    // Menu: the engine passes zero vectors as the listener.
    const float zero[3] = { 0, 0, 0 };
    eng->update_listener(zero, zero, zero, zero);
    peak = 0.0f;
    render(*eng, 512, &peak);
    check(std::isfinite(peak) && peak > 0.02f, "A zero listener (menus) is ignored");

    eng->set_hrtf_enabled(false);
    peak = 0.0f;
    render(*eng, 512, &peak);
    check(std::isfinite(peak) && peak > 0.02f, "sa_hrtf 0 falls back to stereo panning");
    eng->set_master_volume(0.0f);
    render(*eng, 2048);
    peak = 0.0f;
    render(*eng, 512, &peak);
    check(peak < 1e-4f, "sa_volume 0 is silent");
}

void test_threads() {
    std::printf("=== Test 12: Game, audio and raytracing threads at once ===\n");
    auto eng = make_engine(/*reverb=*/true);
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "threads";
    add_box(mesh, 90, 110, -300, 300, -300, 300);
    mesh.submodels.push_back(box_model(1, -30, 30, -2, 2, -50, 50));
    std::string summary;
    eng->load_scene(mesh, summary);

    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> bad{ 0 };
    std::atomic<uint32_t> worst_us{ 0 };
    std::atomic<uint64_t> rendered{ 0 };
    std::thread audio([&] {
        std::vector<float> out(441 * 2);
        while (!stop.load()) {
            const auto t0 = std::chrono::steady_clock::now();
            eng->render_output(out.data(), 441);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            worst_us = std::max(worst_us.load(), static_cast<uint32_t>(us));
            for (float v : out) if (!std::isfinite(v) || std::fabs(v) > 1.0f) ++bad;
            rendered += 441;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread sim([&] {
        while (!stop.load()) {
            eng->run_simulation_sync();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    Camera cam;
    Noise noise;
    float buf[1020];
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    uint32_t iter = 0;
    while (std::chrono::steady_clock::now() < until) {
        cam.yaw(static_cast<float>(iter % 360) * 3.0f);
        publish(*eng, cam);
        while (eng->buffered_frames() < 4000) {
            eng->begin_chunk(1020);
            for (int ch = 0; ch < 24; ++ch) {
                sa::ChannelDescriptor d = source_at(static_cast<float>(ch * 20 - 200), static_cast<float>((iter * 7 + ch) % 300) - 150.0f, 0.0f, 0.01f);
                d.mixer = 1u + (iter + static_cast<uint32_t>(ch) * 5u) / 40u;
                noise.next(buf, 1020);
                for (float& v : buf) v *= 0.05f;
                eng->feed_channel_mono(ch, d, buf, 0, 1020);
            }
            eng->end_chunk(1020);
        }
        if (iter % 50 == 7) eng->notify_channel_freed(static_cast<int>(iter % 24));
        if (iter % 150 == 60) eng->flush_audio();
        if (iter % 200 == 120) { eng->set_enabled(false); eng->set_enabled(true); }
        if (iter % 120 == 50) { eng->unload_scene(); eng->load_scene(mesh, summary); }
        sa::BrushEntityState door{};
        door.entity = 40;
        door.model_index = 1;
        door.solid = iter % 60 < 50;
        door.angles[1] = static_cast<float>(iter % 90);
        if (iter % 3 == 0) eng->update_brush_entities(&door, 1);
        if (iter % 40 == 0) { (void)eng->voice_infos(); (void)eng->brush_entity_infos(); (void)eng->get_stats(); }
        ++iter;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop = true;
    audio.join();
    sim.join();
    std::printf("    %llu frames rendered, worst 10 ms callback %.2f ms\n",
                static_cast<unsigned long long>(rendered.load()), worst_us.load() / 1000.0);
    check(bad == 0 && rendered > 20000, "No NaN, no overflow, no deadlock");
    const auto t0 = std::chrono::steady_clock::now();
    eng->shutdown();
    check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "shutdown() returns promptly");
    check(eng->get_stats().voices == 0, "shutdown() frees every voice");
}

void test_cost() {
    std::printf("=== Test 13: Cost of the callback ===\n");
    for (const int voices : { 8, 32, 64 }) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000; // with resampling
        auto eng = make_engine(false, cfg);
        Camera cam;
        publish(*eng, cam);
        Noise noise;
        float buf[1020];
        for (int c = 0; c < 16; ++c) {
            eng->begin_chunk(1020);
            for (int ch = 0; ch < voices; ++ch) {
                noise.next(buf, 1020);
                eng->feed_channel_mono(ch, source_at(static_cast<float>(ch * 10 - 300), -100.0f, 20.0f), buf, 0, 1020);
            }
            eng->end_chunk(1020);
        }
        render(*eng, 1024);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kBlocks = 50;
        for (int b = 0; b < kBlocks; ++b) {
            cam.yaw(static_cast<float>(b) * 2.0f);
            publish(*eng, cam);
            render(*eng, sa::kDefaultFrameSize);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kBlocks;
        std::printf("    %d moving HRTF voices at 48 kHz: %.3f ms per 2.67 ms block\n", voices, ms);
        check(ms < 2.0, "The callback keeps up");
    }
}

void test_transforms() {
    std::printf("=== Test 14: Brush entity transforms ===\n");
    float worst = 0.0f, worst_inv = 0.0f, worst_ortho = 0.0f;
    for (int k = 0; k < 200; ++k) {
        const float angles[3] = { -180.0f + 37.0f * static_cast<float>(k % 11), -170.0f + 23.0f * static_cast<float>(k % 17),
                                  -60.0f + 31.0f * static_cast<float>(k % 5) };
        const float origin[3] = { 100.0f * static_cast<float>(k), -900.0f + 11.0f * static_cast<float>(k), 64.0f };
        const float local[3] = { 13.0f, -27.0f + static_cast<float>(k), 55.0f };
        // engine.dll AngleVectors (RVA 0xAF040).
        const float p = angles[0] * kPi / 180.0f, y = angles[1] * kPi / 180.0f, r = angles[2] * kPi / 180.0f;
        const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
        const float fwd[3] = { cp * cy, cp * sy, -sp };
        const float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
        const float up[3] = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
        float world[3];
        for (int i = 0; i < 3; ++i) world[i] = origin[i] + local[0] * fwd[i] - local[1] * right[i] + local[2] * up[i];
        const IPLMatrix4x4 m = sa::source_transform_to_ipl(origin, angles);
        const IPLVector3 expected = sa::source_pos_to_ipl(world);
        const IPLVector3 got = sa::transform_point(m, sa::source_pos_to_ipl(local));
        worst = std::max(worst, std::fabs(got.x - expected.x) + std::fabs(got.y - expected.y) + std::fabs(got.z - expected.z));
        const IPLVector3 back = sa::inverse_transform_point(m, got);
        const IPLVector3 l = sa::source_pos_to_ipl(local);
        worst_inv = std::max(worst_inv, std::fabs(back.x - l.x) + std::fabs(back.y - l.y) + std::fabs(back.z - l.z));
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                float dot = 0.0f;
                for (int c = 0; c < 3; ++c) dot += m.elements[c][a] * m.elements[c][b];
                worst_ortho = std::max(worst_ortho, std::fabs(dot - (a == b ? 1.0f : 0.0f)));
            }
        }
    }
    std::printf("    worst deviation %.5f m, inverse %.5f m, orthonormality %.6f\n", worst, worst_inv, worst_ortho);
    check(worst < 2e-3f, "origin + angles place brush vertices where engine.dll's AngleVectors does");
    check(worst_inv < 2e-3f && worst_ortho < 1e-5f, "... as a rigid transform with an exact inverse");
    const float o[3] = { 10, 20, 30 }, none[3] = { 0, 0, 0 };
    const IPLMatrix4x4 t = sa::source_transform_to_ipl(o, none);
    const IPLVector3 at = sa::source_pos_to_ipl(o);
    check(t.elements[0][0] == 1.0f && t.elements[1][1] == 1.0f && t.elements[2][2] == 1.0f && t.elements[3][3] == 1.0f &&
          nearf(t.elements[0][3], at.x) && nearf(t.elements[1][3], at.y) && nearf(t.elements[2][3], at.z),
          "Without angles it is a plain translation");
}

// A minimal VTMB BSP v17: world (floor + sky roof), "*1" = a wooden door, "*2" = a trigger, "*3" = a metal grate.
std::string write_test_bsp() {
    std::vector<unsigned char> lumps[64];
    const auto put = [](std::vector<unsigned char>& v, const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        v.insert(v.end(), b, b + n);
    };
    const auto put_i = [&](int lump, std::int32_t v) { put(lumps[lump], &v, 4); };
    const auto put_s = [&](int lump, std::int16_t v) { put(lumps[lump], &v, 2); };
    const auto put_f = [&](int lump, float v) { put(lumps[lump], &v, 4); };

    const char* names[] = { "concrete/floor01", "tools/toolsskybox", "wood/door01", "tools/toolstrigger", "metal/grate",
                            "tools/toolsnodraw", "maps/sm_pawnshop_2/glass/glassc_-2028_-2475_-70" };
    const int flags[] = { 0, 0x4 | 0x400, 0, 0x480, 0, 0x480, 0 };
    for (int i = 0; i < 7; ++i) {
        put_i(44, static_cast<std::int32_t>(lumps[43].size()));
        put(lumps[43], names[i], std::strlen(names[i]) + 1);
        put_f(2, 0); put_f(2, 0); put_f(2, 0); put_i(2, i); // texdata: reflectivity, name id, sizes
        for (int k = 0; k < 4; ++k) put_i(2, 0);
        for (int k = 0; k < 16; ++k) put_f(6, 0);           // texinfo: vectors, flags, texdata
        put_i(6, flags[i]);
        put_i(6, i);
    }

    struct Box { float lo[3], hi[3]; int tex, bottom_tex; std::int32_t contents; };
    const Box boxes[] = {
        { { -500, -500, -20 }, { 500, 500, 0 }, 0, 0, 1 },
        { { -500, -500, 400 }, { 500, 500, 420 }, 5, 1, 1 }, // sky brush: sky below, nodraw elsewhere
        { { -2, -30, 0 }, { 2, 30, 100 }, 2, 2, 1 },
        { { -50, -50, 0 }, { 50, 50, 100 }, 3, 3, 1 },
        { { -8, -8, -8 }, { 8, 8, 8 }, 4, 4, 1 },
        { { 100, -48, 40 }, { 101, 48, 100 }, 6, 5, 2 },  // a counter's pane, CONTENTS_WINDOW
        { { 200, -48, 40 }, { 201, 48, 100 }, 4, 4, 2 },  // a grate, see-through as well
        { { -300, -48, 40 }, { -290, 48, 100 }, 6, 6, 1 }, // a window painted on a solid wall
        { { -1, -20, 50 }, { 1, 20, 90 }, 6, 6, 2 },      // the pane of a door
    };
    const int model_of_box[] = { 0, 0, 1, 2, 3, 0, 0, 0, 1 };
    for (const Box& b : boxes) {
        put_i(18, static_cast<std::int32_t>(lumps[19].size() / 8));
        put_i(18, 6);
        put_i(18, b.contents);
        for (int side = 0; side < 6; ++side) {
            const int axis = side / 2;
            const bool high = (side & 1) == 0;
            float n[3] = { 0, 0, 0 };
            n[axis] = high ? 1.0f : -1.0f;
            put_s(19, static_cast<std::int16_t>(lumps[1].size() / 20));
            put_s(19, static_cast<std::int16_t>(axis == 2 && !high ? b.bottom_tex : b.tex));
            put_s(19, 0);
            put_s(19, 0);
            put_f(1, n[0]); put_f(1, n[1]); put_f(1, n[2]);
            put_f(1, high ? b.hi[axis] : -b.lo[axis]);
            put_i(1, axis);
        }
    }
    for (int m = 0; m < 4; ++m) {
        const int first = static_cast<int>(lumps[17].size() / 2);
        int count = 0;
        for (int b = 0; b < 9; ++b) {
            if (model_of_box[b] != m) continue;
            put_s(17, static_cast<std::int16_t>(b));
            ++count;
        }
        unsigned char leaf[32] = {};
        const std::uint16_t f16 = static_cast<std::uint16_t>(first), c16 = static_cast<std::uint16_t>(count);
        std::memcpy(leaf + 24, &f16, 2);
        std::memcpy(leaf + 26, &c16, 2);
        put(lumps[10], leaf, 32);
        unsigned char empty[32] = {};
        put(lumps[10], empty, 32);
        unsigned char node[32] = {};
        const std::int32_t c0 = -1 - 2 * m, c1 = -2 - 2 * m;
        std::memcpy(node + 4, &c0, 4);
        std::memcpy(node + 8, &c1, 4);
        put(lumps[5], node, 32);
        unsigned char model[48] = {};
        const std::int32_t head = m;
        std::memcpy(model + 36, &head, 4);
        put(lumps[14], model, 48);
    }

    struct Entry { std::int32_t ofs, len, version, fourcc; };
    std::vector<unsigned char> file(8 + 64 * sizeof(Entry) + 4, 0);
    std::memcpy(file.data(), "VBSP", 4);
    const std::int32_t version = 17;
    std::memcpy(file.data() + 4, &version, 4);
    for (int i = 0; i < 64; ++i) {
        const Entry e{ static_cast<std::int32_t>(file.size()), static_cast<std::int32_t>(lumps[i].size()), 0, 0 };
        std::memcpy(file.data() + 8 + static_cast<std::size_t>(i) * sizeof(Entry), &e, sizeof(e));
        file.insert(file.end(), lumps[i].begin(), lumps[i].end());
    }
    const std::string path = "build\\test_submodels.bsp";
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return std::string();
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return path;
}

void test_bsp_submodels() {
    std::printf("=== Test 15: Brush models and sky faces from a BSP ===\n");
    const std::string path = write_test_bsp();
    sa::AcousticMeshData mesh;
    std::string summary;
    const bool ok = sa::extract_acoustic_mesh_from_bsp_file(path.c_str(), mesh, summary);
    std::printf("    %s\n", summary.c_str());
    check(ok && mesh.triangles.size() == 4 * 12, "The world holds its own solid brushes only: doors are not baked into it");
    float glass_lo = 1e9f, glass_hi = -1e9f;
    for (const IPLVector3& p : mesh.glass_vertices) {
        glass_lo = std::min(glass_lo, -p.z);
        glass_hi = std::max(glass_hi, -p.z);
    }
    check(mesh.glass_triangles.size() == 12 && nearf(glass_lo, 100 * sa::kSourceToMeters, 1e-3f) && nearf(glass_hi, 101 * sa::kSourceToMeters, 1e-3f),
          "Panes are see-through and of glass: no grate, no painted window, with all their faces");
    check(mesh.submodels.size() == 2 && mesh.submodels[0].glass_triangles.size() == 12 && mesh.submodels[1].glass_triangles.empty(),
          "... those of doors, too");
    check(mesh.submodels.size() == 2 && mesh.submodels[0].model_index == 1 && mesh.submodels[1].model_index == 3,
          "Solid brush models *1 and *3 are extracted, the trigger *2 is not");
    if (mesh.submodels.size() == 2) {
        check(mesh.submodels[0].triangles.size() == 24 && mesh.submodels[0].material_indices.size() == 24 &&
              mesh.submodels[0].material_indices[0] == sa::kMatWood && mesh.submodels[1].material_indices[0] == sa::kMatMetal,
              "... with their materials");
        IPLVector3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
        for (const IPLVector3& v : mesh.submodels[0].vertices) {
            lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
            hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
        }
        check(nearf(hi.x - lo.x, 60 * sa::kSourceToMeters, 1e-3f) && nearf(hi.y - lo.y, 100 * sa::kSourceToMeters, 1e-3f) &&
              nearf(hi.z - lo.z, 4 * sa::kSourceToMeters, 1e-3f) && nearf(lo.y, 0.0f, 1e-3f),
              "... relative to their own origin");
    }
    check(mesh.sky_triangles.size() == 2, "Only the face that shows the sky counts as sky, not its nodraw back sides");
    std::remove(path.c_str());
}

void run_ticks(sa::SteamAudioEngine& eng, int n) {
    for (int i = 0; i < n; ++i) eng.run_simulation_sync();
}

void test_dynamic_doors() {
    std::printf("=== Test 16: Doors as instanced meshes ===\n");
    auto eng = make_engine(/*reverb=*/true);
    if (!eng->is_initialized()) return;
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "test_door";
    add_room(mesh);
    // A wall at x = 100 with a doorway y -40..40, z -300..100.
    add_box(mesh, 98, 108, -400, -40, -300, 300);
    add_box(mesh, 98, 108, 40, 400, -300, 300);
    add_box(mesh, 98, 108, -40, 40, 100, 300);
    // The door hangs on its hinge (y = 0 of the model) and fills the doorway.
    mesh.submodels.push_back(box_model(7, -2, 2, 0, 80, -300, 100));
    std::string summary;
    check(eng->load_scene(mesh, summary), ("load_scene: " + summary).c_str());

    Camera cam;
    publish(*eng, cam);
    const sa::ChannelDescriptor behind = source_at(250.0f, 0.0f, 0.0f);
    sa::ChannelDescriptor creak = source_at(103.0f, 0.0f, 0.0f); // the door's own sound, from its middle
    creak.entnum = 55;
    creak.mixer = 2;
    Noise noise;
    float buf[512];
    eng->begin_chunk(512);
    noise.next(buf, 512);
    eng->feed_channel_mono(0, behind, buf, 0, 512);
    eng->feed_channel_mono(1, creak, buf, 0, 512);
    eng->end_chunk(512);
    const auto occlusion = [&](int ch) {
        eng->run_simulation_sync();
        IPLDirectEffectParams p{};
        return eng->get_channel_direct_params(ch, p) ? p.occlusion : -1.0f;
    };
    const float no_door = occlusion(0);

    sa::BrushEntityState door{};
    door.entity = 55;
    door.model_index = 7;
    door.origin[0] = 103.0f;
    door.origin[1] = -40.0f;
    door.solid = true;
    eng->update_brush_entities(&door, 1);
    const float closed = occlusion(0);
    IPLDirectEffectParams closed_params{};
    eng->get_channel_direct_params(0, closed_params);
    const float own = occlusion(1);
    const sa::EngineStats st_closed = eng->get_stats();
    check(st_closed.brush_models == 1 && st_closed.brush_entities == 1, "The door stands in the scene");

    door.angles[1] = 90.0f; // swings open around its hinge
    eng->update_brush_entities(&door, 1);
    const float open = occlusion(0);
    door.angles[1] = 0.0f;
    door.origin[2] = 420.0f; // a lift door that slides up
    eng->update_brush_entities(&door, 1);
    const float slid = occlusion(0);
    door.origin[2] = 0.0f;
    eng->update_brush_entities(&door, 1);
    const float again = occlusion(0);
    std::printf("    occlusion of a source behind: no door %.2f | closed %.2f (transmission %.2f/%.2f/%.2f) | swung open %.2f |"
                " slid away %.2f | closed again %.2f | the door's own sound %.2f\n",
                no_door, closed, closed_params.transmission[0], closed_params.transmission[1], closed_params.transmission[2],
                open, slid, again, own);
    check(no_door > 0.9f && closed < 0.1f, "A closed door blocks the sound behind it");
    check(closed_params.transmission[0] > closed_params.transmission[2] && closed_params.transmission[0] < 0.9f,
          "... and lets wood's share of it through, more bass than treble");
    check(open > 0.9f && slid > 0.9f, "Swinging or sliding it open clears the way");
    check(again < 0.1f, "Closing it blocks it again");
    check(own > 0.9f, "A door does not muffle its own sound");

    // Nothing moved: no transform update, no scene commit.
    const sa::EngineStats before = eng->get_stats();
    for (int i = 0; i < 5; ++i) {
        eng->update_brush_entities(&door, 1);
        eng->run_simulation_sync();
    }
    door.origin[0] += 0.01f; // below the threshold
    eng->update_brush_entities(&door, 1);
    eng->run_simulation_sync();
    const sa::EngineStats after = eng->get_stats();
    check(after.scene_commits == before.scene_commits && after.brush_moves == before.brush_moves,
          "Doors that stand still cost no scene commit");

    door.solid = false; // broken, or made non-solid
    eng->update_brush_entities(&door, 1);
    check(occlusion(0) > 0.9f && eng->get_stats().brush_entities == 0, "A door that is no longer solid is gone from the scene");
    door.solid = true;
    sa::BrushEntityState two[2] = { door, door }; // the same model twice, and one that does not exist
    two[1].entity = 56;
    two[1].origin[1] = 200.0f;
    eng->update_brush_entities(two, 2);
    check(occlusion(0) < 0.1f && eng->get_stats().brush_entities == 2, "Several entities can use one brush model");
    Camera away;
    away.org[0] = 4000.0f; // 100 m away, and back
    publish(*eng, away);
    run_ticks(*eng, 1);
    check(eng->get_stats().brush_entities == 0, "Doors far from the listener leave the scene");
    publish(*eng, cam);
    check(occlusion(0) < 0.1f && eng->get_stats().brush_entities == 2, "... and are back when the player is");
    mesh.submodels.push_back(box_model(8, -3, 3, -3, 3, -60, 60)); // a pipe
    two[1].model_index = 999;
    eng->update_brush_entities(two, 2);
    run_ticks(*eng, 1);
    check(eng->get_stats().brush_entities == 1, "Unknown brush models are ignored");

    eng->set_dynamic_doors(false);
    check(occlusion(0) > 0.9f, "sa_doors 0 takes the doors out");
    eng->set_dynamic_doors(true);
    check(occlusion(0) < 0.1f, "... and 1 puts them back");

    eng->unload_scene();
    check(eng->get_stats().brush_entities == 0 && eng->brush_entity_infos().empty(), "Level shutdown releases every instance");
    check(eng->load_scene(mesh, summary), "The next level loads");
    check(occlusion(0) > 0.9f, "... and starts without the old level's doors");
    sa::BrushEntityState pipe = door;
    pipe.entity = 77;
    pipe.model_index = 8;
    const sa::BrushEntityState known[2] = { door, pipe };
    eng->update_brush_entities(known, 2);
    check(occlusion(0) < 0.1f && eng->get_stats().brush_entities == 1, "... until its own are known (a thin pipe is no obstacle)");
    check(eng->load_scene(mesh, summary) && occlusion(0) < 0.1f, "sa_reload_bsp keeps the doors");
    eng->shutdown();

    // Many doors: the raytracing thread has to cope with a hub map.
    auto many = make_engine(true);
    sa::AcousticMeshData hub = mesh;
    many->load_scene(hub, summary);
    publish(*many, cam);
    many->begin_chunk(512);
    for (int c = 0; c < 24; ++c) many->feed_channel_mono(c, source_at(150.0f, -200.0f + 16.0f * static_cast<float>(c), 0.0f), buf, 0, 512);
    many->end_chunk(512);
    run_ticks(*many, 2);
    const sa::EngineStats none = many->get_stats();
    std::vector<sa::BrushEntityState> lots;
    for (int i = 0; i < 150; ++i) {
        sa::BrushEntityState b{};
        b.entity = 100 + i;
        b.model_index = 7;
        b.solid = true;
        b.origin[0] = -380.0f + 5.0f * static_cast<float>(i);
        b.origin[1] = 300.0f;
        lots.push_back(b);
    }
    many->update_brush_entities(lots.data(), lots.size());
    run_ticks(*many, 1);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        lots[0].angles[1] = static_cast<float>(i * 5 + 5);
        many->update_brush_entities(lots.data(), lots.size());
        many->run_simulation_sync();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
    const sa::EngineStats full = many->get_stats();
    std::printf("    0 -> 150 doors: direct %.2f -> %.2f ms, reflections %.2f -> %.2f ms; whole step with a moving door %.1f ms\n",
                none.sim_us_direct / 1000.0, full.sim_us_direct / 1000.0, none.sim_us_reflections / 1000.0,
                full.sim_us_reflections / 1000.0, ms);
    check(full.brush_entities == 48 && ms < 120.0, "Of 150 doors the 48 nearest take part: affordable for the raytracing thread");
}

// Plays `seconds` of a 2D tone at the engine's output rate and returns the signal-to-error ratio in dB.
double tone_snr(sa::SteamAudioEngine& eng, float freq, int callback) {
    sa::ChannelDescriptor music{};
    music.active = true;
    music.reverb_gain = 0.0f;
    music.mixer = 1;
    Tone tone;
    tone.freq = freq;
    std::vector<float> out;
    std::vector<float> buf(static_cast<std::size_t>(callback) * 2);
    while (out.size() < 24000) {
        while (eng.buffered_frames() < 4000) feed(eng, 0, music, tone, 500);
        eng.render_output(buf.data(), static_cast<uint32_t>(callback));
        for (int i = 0; i < callback; ++i) out.push_back(buf[static_cast<std::size_t>(i) * 2]);
    }
    // Least-squares fit of the tone to the settled part: whatever is left over is error.
    const double w = 2.0 * 3.14159265358979 * freq / eng.output_rate();
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0, xx = 0;
    const std::size_t from = 4000, to = 22000;
    for (std::size_t i = from; i < to; ++i) {
        const double s = std::sin(w * static_cast<double>(i)), c = std::cos(w * static_cast<double>(i));
        ss += s * s; cc += c * c; sc += s * c;
        xs += out[i] * s; xc += out[i] * c; xx += static_cast<double>(out[i]) * out[i];
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    const double fit = a * xs + b * xc;
    const double err = std::max(xx - fit, 1e-15 * xx);
    const double level = std::sqrt(a * a + b * b);
    std::printf("    %d Hz tone at %d Hz, callback %d: level %.4f, error %.0f dB\n", static_cast<int>(freq), eng.output_rate(),
                callback, level, 10.0 * std::log10(err / fit));
    return 10.0 * std::log10(fit / err);
}

void test_output_rate() {
    std::printf("=== Test 17: 48 kHz output, 128-frame blocks, pose responsiveness ===\n");
    sa::EngineConfig cfg{};
    cfg.sample_rate = 48000;
    auto eng = make_engine(false, cfg);
    check(eng->output_rate() == 48000 && eng->frame_size() == 128, "Steam Audio runs at 48 kHz in 128-frame blocks (2.7 ms)");
    check(tone_snr(*eng, 440.0f, 480) > 80.0, "A 440 Hz tone comes out clean (bus resampled from 44.1 kHz)");
    auto hi = make_engine(false, cfg);
    check(tone_snr(*hi, 9000.0f, 441) > 60.0, "... a 9 kHz tone too, with odd callback sizes");

    // Same pitch, same speed: 1 s of output consumes 1 s of dry audio.
    Tone tone;
    sa::ChannelDescriptor music{};
    music.active = true;
    music.mixer = 1;
    auto speed = make_engine(false, cfg);
    feed(*speed, 0, music, tone, 10000);
    render(*speed, 4800);
    const uint32_t b0 = speed->buffered_frames();
    uint32_t fed = 0;
    for (int i = 0; i < 100; ++i) {
        feed(*speed, 0, music, tone, 441);
        fed += 441;
        render(*speed, 480);
    }
    const int drift = static_cast<int>(b0 + fed - speed->buffered_frames()) - 44100;
    std::printf("    1 s of output consumed %d dry frames more than 44100\n", drift);
    check(std::abs(drift) <= 128, "Playback speed is exact");

    // The pose of the block, not of the paint: at 48 kHz with 200 ms queued.
    auto turn = make_engine(false, cfg);
    Camera cam;
    publish(*turn, cam);
    Noise noise;
    feed(*turn, 5, source_at(0.0f, -60.0f, 0.0f, dist_mult_for(60, 50)), noise, 8820);
    render(*turn, 2048);
    const Energy before = render(*turn, 1024);
    cam.yaw(180.0f);
    publish(*turn, cam);
    render(*turn, 128);
    const Energy second = render(*turn, 256);
    const Energy settled = render(*turn, 1024);
    const auto ild = [](const Energy& e) { return 10.0 * std::log10(e.r / e.l); };
    std::printf("    right-left: before %.1f dB | 2.7-8 ms after a half turn %.1f dB | settled %.1f dB\n",
                ild(before), ild(second), ild(settled));
    check(ild(before) > 3.0 && ild(settled) < -3.0 && std::fabs(ild(second) - ild(settled)) < 3.0,
          "A half turn is complete one 2.7 ms block later");

    check(sa::block_for_period(480) == 120 && sa::block_for_period(441) == 147 && sa::block_for_period(528) == 132 &&
          sa::block_for_period(128) == 128 && sa::block_for_period(256) == 128 && sa::block_for_period(960) == 120,
          "Blocks divide the device's period: nothing rendered early is left over");
    check(sa::block_for_period(0) == 128 && sa::block_for_period(101) == 101 && sa::block_for_period(97) == 97 &&
          sa::block_for_period(191) == 128, "... or fall back to 128");
    const sa::EngineStats st = turn->get_stats();
    check(st.output_rate == 48000 && st.frame_size == 128 && st.pose_age_us < 5000000u && st.render_us_max > 0,
          "The latency breakdown for sa_status is filled in");
}

void test_resampler() {
    std::printf("=== Test 18: Resampler ===\n");
    sa::Resampler rs;
    rs.init(160);
    std::vector<float> in(400);
    for (int i = 0; i < 400; ++i) in[static_cast<std::size_t>(i)] = std::sin(0.2f * static_cast<float>(i));
    float out[160];
    rs.prepare(0.0, 1.0, 128);
    rs.process(in.data(), out);
    float worst = 0.0f;
    for (int i = 0; i < 128; ++i) worst = std::max(worst, std::fabs(out[i] - in[static_cast<std::size_t>(i + sa::Resampler::kHistory)]));
    check(worst < 2e-3f, "On whole frames it returns the input");
    // Two blocks in a row line up.
    const double step = 44100.0 / 48000.0;
    float a[256], b[256];
    double frac = 0.0;
    int pos = 0;
    for (int blk = 0; blk < 2; ++blk) {
        rs.prepare(frac, step, 128);
        rs.process(in.data() + pos, a + blk * 128);
        const double end = frac + step * 128;
        pos += static_cast<int>(end);
        frac = end - static_cast<int>(end);
    }
    for (int i = 0; i < 256; ++i) {
        b[i] = std::sin(0.2f * static_cast<float>(sa::Resampler::kHistory + step * i));
    }
    worst = 0.0f;
    for (int i = 0; i < 256; ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
    std::printf("    worst deviation from the ideal 44.1 -> 48 kHz curve %.5f\n", worst);
    check(worst < 2e-3f, "Blocks join without a seam at fractional positions");
    std::vector<float> dc(400, 1.0f);
    rs.prepare(0.37, step, 128);
    rs.process(dc.data(), out);
    worst = 0.0f;
    for (int i = 0; i < 128; ++i) worst = std::max(worst, std::fabs(out[i] - 1.0f));
    check(worst < 1e-5f, "Level is preserved exactly");
}

struct RoomEnergy { double direct, tail; };

// 100 ms of noise through a room with the given channel; energy while it plays and in the 300 ms after it stopped.
RoomEnergy play_in_room(sa::SteamAudioEngine& eng, const sa::ChannelDescriptor& d, double* early = nullptr) {
    Noise noise;
    const int rate = eng.output_rate();
    feed(eng, 0, d, noise, 4410);
    eng.run_simulation_sync();
    feed_silence(eng, 22050);
    eng.run_simulation_sync();
    const Energy direct = render(eng, rate / 10);
    render(eng, rate / 50); // the 2D/HRTF signal is over
    const Energy e1 = render(eng, rate / 10);
    const Energy e2 = render(eng, rate / 5);
    if (early) *early = e1.l + e1.r;
    eng.notify_channel_freed(0);
    return { direct.l + direct.r, e1.l + e1.r + e2.l + e2.r };
}

sa::AcousticMeshData room_mesh() {
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "room";
    add_room(mesh);
    return mesh;
}

void test_reverb_modes() {
    std::printf("=== Test 19: Hybrid and parametric reverb ===\n");
    const sa::AcousticMeshData mesh = room_mesh();
    std::string summary;
    Camera cam;
    RoomEnergy result[2] = {};
    for (int mode = 0; mode < 2; ++mode) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.reverb_mode = mode == 1 ? sa::ReverbMode::kHybrid : sa::ReverbMode::kParametric;
        auto eng = make_engine(true, cfg);
        check(eng->get_stats().reverb_mode == cfg.reverb_mode, mode == 1 ? "Hybrid reverb is created" : "Parametric reverb is created");
        eng->load_scene(mesh, summary);
        publish(*eng, cam);
        run_ticks(*eng, 3);
        result[mode] = play_in_room(*eng, source_at(200.0f, 0.0f, 0.0f, 0.005f));
        const sa::EngineStats st = eng->get_stats();
        std::printf("    %s: tail/direct %.1f dB, RT60 %.2f s, reflections %.1f ms, worst block %.2f ms\n",
                    mode == 1 ? "hybrid" : "parametric", 10.0 * std::log10(result[mode].tail / result[mode].direct),
                    st.reverb_time[0], st.sim_us_reflections / 1000.0, st.render_us_max / 1000.0);
        check(result[mode].tail > result[mode].direct * 1e-5 && result[mode].tail < result[mode].direct * 0.2,
              "The room answers, and well below the sound itself");
        check(st.reverb_time[0] > 0.1f && st.reverb_time[0] < 10.0f, "Plausible decay time");

        // Long run: stays finite and stable, decays to silence.
        feed_silence(*eng, 30000);
        float peak = 0.0f;
        render(*eng, 24000, &peak);
        eng->flush_audio();
        render(*eng, 2048);
        peak = 0.0f;
        render(*eng, 2048, &peak);
        check(std::isfinite(peak) && peak < 1e-4f, "A level change leaves no reverb tail behind");
        eng->run_simulation_sync();
        const RoomEnergy again = play_in_room(*eng, source_at(200.0f, 0.0f, 0.0f, 0.005f));
        check(again.tail > again.direct * 1e-5, "... and the next level has reverb again");

        eng->set_reverb_enabled(false);
        render(*eng, 48000);
        const RoomEnergy off = play_in_room(*eng, source_at(200.0f, 0.0f, 0.0f, 0.005f));
        check(off.tail < off.direct * 1e-7, "sa_reverb 0 is dry");
    }
    check(std::fabs(10.0 * std::log10(result[1].tail / result[0].tail)) < 12.0, "Both modes are similarly loud");

    // Early reflections are the point of it: a wall to the left answers on the left.
    sa::EngineConfig cfg{};
    cfg.sample_rate = 48000;
    cfg.reverb_wet = 2.0f;
    auto eng = make_engine(true, cfg);
    sa::AcousticMeshData one_wall;
    one_wall.materials = sa::default_acoustic_materials();
    one_wall.map_name = "one_wall";
    add_box(one_wall, -600, 600, 100, 120, -600, 600); // to the left
    eng->load_scene(one_wall, summary);
    publish(*eng, cam);
    run_ticks(*eng, 3);
    sa::ChannelDescriptor own{};
    own.active = true;
    own.mixer = 1;
    own.reverb_gain = 1.0f;
    Noise noise;
    feed(*eng, 0, own, noise, 441);
    feed_silence(*eng, 22050);
    render(*eng, 640);
    const Energy echo = render(*eng, 2400);
    std::printf("    a wall on the left answers with left-right %.1f dB\n", 10.0 * std::log10(echo.l / echo.r));
    check(echo.l > echo.r * 1.3, "Early reflections come from the wall they bounce off");
}

void test_dialogue() {
    std::printf("=== Test 20: Dialogue is dry and never behind a wall ===\n");
    sa::AcousticMeshData mesh = room_mesh();
    add_box(mesh, 60, 64, -80, 80, -300, 300); // a bar between the camera and the speaker
    std::string summary;
    Camera cam;

    sa::ChannelSnapshot c{};
    c.name = "*character/dlg/bar/line1.mp3";
    c.entnum = 300;
    c.entchannel = sa::kChanStream;
    c.origin[0] = 100.0f;
    c.dist_mult = dist_mult_for(80, 36);
    c.master_vol = 255;
    c.is_voice = true;
    c.distance = 100.0f;
    c.mouth_height = 64.0f;
    c.skip_collide = true;
    const sa::ChannelDescriptor line = sa::describe_channel(c);
    check(line.dialogue && line.no_occlusion && line.spatial_3d && line.is_voice, "A conversation line (SND 0x280) is recognized");
    check(nearf(line.origin[2], 64.0f), "A voice comes from the mouth, not from the feet");
    check(line.reverb_gain > 0.2f && line.reverb_gain <= 0.5f, "Voices get at most dsp_voice_max of the room, as in engine.dll");
    c.skip_collide = false;
    const sa::ChannelDescriptor shout = sa::describe_channel(c);
    check(!shout.dialogue && !shout.no_occlusion, "A bark or a shout (SND 0x80) can be behind a wall");
    c.entnum = 1;
    c.own = true;
    check(!sa::describe_channel(c).spatial_3d, "The player's own voice stays in the head");

    struct Result { RoomEnergy e; float occ; };
    const auto play = [&](const sa::ChannelDescriptor& d) {
        auto eng = make_engine(true);
        eng->load_scene(mesh, summary);
        publish(*eng, cam);
        run_ticks(*eng, 3);
        sa::ChannelDescriptor x = d;
        x.mixer = 9;
        Result r{};
        r.e = play_in_room(*eng, x);
        return r;
    };
    sa::ChannelDescriptor effect = shout;
    effect.is_voice = false;
    effect.reverb_gain = 0.6f;
    sa::ChannelDescriptor open_line = line;
    const Result said = play(open_line), shouted = play(shout), noise = play(effect);
    const auto db = [](double a, double b) { return 10.0 * std::log10(a / b); };
    std::printf("    behind a thin wall: line %.5f (reverb %.1f dB), shout %.5f (reverb %.1f dB), effect reverb %.1f dB\n",
                said.e.direct, db(said.e.tail, said.e.direct), shouted.e.direct, db(shouted.e.tail, shouted.e.direct),
                db(noise.e.tail, noise.e.direct));
    check(said.e.direct > shouted.e.direct * 1.5, "The line is at full level where a shout is muffled by the wall");
    check(said.e.tail < said.e.direct * 1e-4, "The room stays 40 dB below the spoken line: no echo");
    check(said.e.tail < noise.e.tail * 0.1 && said.e.tail < shouted.e.tail * 0.5, "... below that of a shout behind the wall, and far below that of effects");

    auto eng = make_engine(true);
    eng->load_scene(mesh, summary);
    publish(*eng, cam);
    Noise n;
    sa::ChannelDescriptor x = line;
    x.mixer = 5;
    feed(*eng, 3, x, n, 4410);
    run_ticks(*eng, 1);
    render(*eng, 1024);
    IPLDirectEffectParams p{};
    check(!eng->get_channel_direct_params(3, p), "Dialogue is not even ray traced");
    const std::vector<sa::VoiceInfo> infos = eng->voice_infos();
    check(infos.size() == 1 && infos[0].slot == 3 && infos[0].dialogue && infos[0].is_voice && infos[0].no_occlusion &&
          infos[0].occlusion > 0.99f && infos[0].reverb_send < 0.05f && std::fabs(infos[0].distance_m - 3.02f) < 0.1f,
          "sa_debug shows it: dialogue, unoccluded, almost no reverb");
}

void test_channel_model() {
    std::printf("=== Test 21: What a channel_t means ===\n");
    sa::ChannelSnapshot c{};
    c.name = "music/santa_monica.mp3";
    c.entnum = 0;
    c.master_vol = 255;
    c.volumes[0] = c.volumes[1] = 127; // S_SpatializeChannel: half per side for a centred sound
    sa::ChannelDescriptor d = sa::describe_channel(c);
    check(!d.spatial_3d && nearf(d.master_gain, 1.0f) && d.reverb_gain == 0.0f && d.weather == 0.0f,
          "2D at full volume plays at full volume, without reverb");
    c.volumes[0] = c.volumes[1] = 50;
    d = sa::describe_channel(c);
    check(std::fabs(d.master_gain - 0.396f) < 0.005f, "2D sounds follow the volume the engine computed (fades, snd_gain)");
    c.volumes[0] = c.volumes[1] = c.volumes[2] = c.volumes[3] = 25; // surround splits front and rear
    check(std::fabs(sa::describe_channel(c).master_gain - 0.396f) < 0.005f, "... with surround speakers too");
    c.volumes[0] = c.volumes[1] = c.volumes[2] = c.volumes[3] = 0;
    check(sa::describe_channel(c).master_gain == 0.0f, "... down to silence");
    c.volumes[0] = c.volumes[1] = 127;
    c.duck = 0.5f;
    check(nearf(sa::describe_channel(c).master_gain, 0.5f), "Music is ducked under voices");
    c.no_duck = true;
    check(nearf(sa::describe_channel(c).master_gain, 1.0f), "... unless the sound says no");

    sa::ChannelSnapshot e{};
    e.name = "physics/metal.wav";
    e.entnum = 40;
    e.dist_mult = dist_mult_for(75, 36);
    e.master_vol = 128;
    e.volumes[0] = 3;
    e.volumes[1] = 90;
    e.distance = 300.0f;
    d = sa::describe_channel(e);
    check(d.spatial_3d && std::fabs(d.master_gain - 0.502f) < 0.003f, "3D sounds use master_vol: distance and pan are ours");
    check(d.reverb_gain > 0.6f && d.reverb_gain < 0.75f, "Room share rises with distance as in SND_GetDspMix");
    e.dry = true;
    check(sa::describe_channel(e).reverb_gain == 0.0f, "Dry files ('#', '>') stay dry");
    e.dry = false;
    e.skip_collide = true;
    d = sa::describe_channel(e);
    check(d.no_occlusion && !d.dialogue, "flag_skip_collide ambients are never occluded, and are no dialogue");
    e.origin[1] = std::nanf("");
    check(!sa::describe_channel(e).spatial_3d, "A broken position is never spatialized");
    const sa::GainModel gm;
    check(nearf(sa::engine_room_share(0.0f, dist_mult_for(80, 36), false, gm), 0.6f, 0.01f) &&
          nearf(sa::engine_room_share(5000.0f, dist_mult_for(80, 36), false, gm), 0.99f, 0.01f) &&
          nearf(sa::engine_room_share(0.0f, dist_mult_for(65, 36), false, gm), 0.51f, 0.01f) &&
          nearf(sa::engine_room_share(5000.0f, dist_mult_for(80, 36), true, gm), 0.5f, 0.01f),
          "engine_room_share reproduces SND_GetDspMix");

    check(sa::is_weather_sound("area/Santa_Monica/rain_light_loop.wav") && sa::is_weather_sound(")Environmental\\Weather\\rain1.wav") &&
          sa::is_weather_sound("ambient/thunder_clap.wav") && sa::is_weather_sound("area/oceanhouse/rain_moderate.wav") &&
          sa::is_weather_sound("Weather/StormyNight_thunderstorm.wav") && sa::is_weather_sound("x/rainy_street.wav"),
          "Rain and thunder are recognized by name");
    check(!sa::is_weather_sound("vehicles/train_pass.wav") && !sa::is_weather_sound("sewer/drain_loop.wav") &&
          !sa::is_weather_sound("character/brain.wav") && !sa::is_weather_sound("music/rain_theme.mp3") &&
          !sa::is_weather_sound("ui/rain.wav") && !sa::is_weather_sound("terrain/step.wav") &&
          !sa::is_weather_sound("*character/dlg/rain.mp3") && !sa::is_weather_sound("") && !sa::is_weather_sound(nullptr),
          "... trains, drains, brains, music and dialogue are not");
}

sa::ChannelSnapshot rain_snapshot() {
    sa::ChannelSnapshot r{}; // sm_hub_1: ambient_generic "rain_sounds", spawnflags 17 (play everywhere), health 4
    r.name = "area/Santa_Monica/rain_light_loop.wav";
    r.entnum = 120;
    r.master_vol = 102;
    r.volumes[0] = r.volumes[1] = 51;
    r.is_looped = true;
    return r;
}

sa::AcousticMeshData street_mesh() {
    // Street with a garage: floor, sky above, a roof over x > 100 with walls, open towards the street (x < 100).
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "street";
    add_box(mesh, -1000, 1000, -1000, 1000, -20, 0);
    add_box(mesh, 100, 900, -300, 300, 150, 170);   // roof
    add_box(mesh, 890, 900, -300, 300, 0, 150);     // back wall
    add_box(mesh, 100, 900, -310, -300, 0, 170);    // side walls
    add_box(mesh, 100, 900, 300, 310, 0, 170);
    const sa::AcousticBrushPlane sky[6] = {
        {  1, 0, 0, 1000, 0 }, { -1, 0, 0, 1000, 0 }, { 0, 1, 0, 1000, 0 }, { 0, -1, 0, 1000, 0 },
        {  0, 0, 1, 820, 0 },  { 0, 0, -1, -800, 0, true },
    };
    sa::clip_sky_brush(sky, 6, mesh);
    const sa::AcousticBrushPlane east[6] = { // and the sky box's sides
        { 1, 0, 0, 1020, 0 }, { -1, 0, 0, -1000, 0, true }, { 0, 1, 0, 1000, 0 }, { 0, -1, 0, 1000, 0 }, { 0, 0, 1, 820, 0 }, { 0, 0, -1, 0, 0 },
    };
    sa::clip_sky_brush(east, 6, mesh);
    const sa::AcousticBrushPlane west[6] = {
        { 1, 0, 0, -1000, 0, true }, { -1, 0, 0, 1020, 0 }, { 0, 1, 0, 1000, 0 }, { 0, -1, 0, 1000, 0 }, { 0, 0, 1, 820, 0 }, { 0, 0, -1, 0, 0 },
    };
    sa::clip_sky_brush(west, 6, mesh);
    const sa::AcousticBrushPlane north[6] = {
        { 1, 0, 0, 1000, 0 }, { -1, 0, 0, 1000, 0 }, { 0, 1, 0, 1020, 0 }, { 0, -1, 0, -1000, 0, true }, { 0, 0, 1, 820, 0 }, { 0, 0, -1, 0, 0 },
    };
    sa::clip_sky_brush(north, 6, mesh);
    const sa::AcousticBrushPlane south[6] = {
        { 1, 0, 0, 1000, 0 }, { -1, 0, 0, 1000, 0 }, { 0, 1, 0, -1000, 0, true }, { 0, -1, 0, 1020, 0 }, { 0, 0, 1, 820, 0 }, { 0, 0, -1, 0, 0 },
    };
    sa::clip_sky_brush(south, 6, mesh);
    return mesh;
}

void test_weather() {
    std::printf("=== Test 22: Rain level, roofs and sky exposure ===\n");
    sa::ChannelDescriptor rain = sa::describe_channel(rain_snapshot());
    rain.mixer = 1;
    check(!rain.spatial_3d && rain.weather > 0.99f && rain.reverb_gain == 0.0f, "The hub's rain is 2D weather");
    sa::ChannelSnapshot indoor = rain_snapshot();
    indoor.master_vol = 25;
    indoor.volumes[0] = indoor.volumes[1] = 12;
    check(sa::describe_channel(indoor).weather < 0.01f, "Rain that the map already mixed for indoors (10 %) is left alone");
    sa::ChannelSnapshot sewer = rain_snapshot();
    sewer.dist_mult = 0.01f;
    check(sa::describe_channel(sewer).weather == 0.0f, "Positioned rain is ray traced like everything else");

    sa::AcousticMeshData mesh = street_mesh();
    check(mesh.sky_triangles.size() == 10, "Sky faces are collected");
    // A shutter that closes the garage, as a brush model.
    mesh.submodels.push_back(box_model(1, -4, 4, -300, 300, 0, 150));

    sa::SkyProbe probe;
    probe.build(mesh);
    const IPLVector3 street = sa::source_pos_to_ipl(-400.0f, 0.0f, 64.0f);
    const IPLVector3 gate = sa::source_pos_to_ipl(140.0f, 0.0f, 64.0f);
    const IPLVector3 deep = sa::source_pos_to_ipl(700.0f, 0.0f, 64.0f);
    const float e_street = probe.exposure(street, nullptr, 0), e_gate = probe.exposure(gate, nullptr, 0), e_deep = probe.exposure(deep, nullptr, 0);
    sa::TriangleBvh shutter_bvh;
    shutter_bvh.build(mesh.submodels[0].vertices, mesh.submodels[0].triangles, mesh.submodels[0].triangles.size());
    const float at[3] = { 100, 0, 0 }, none[3] = { 0, 0, 0 };
    const sa::SkyProbe::Mover shutter{ &shutter_bvh, sa::source_transform_to_ipl(at, none) };
    const float e_shut = probe.exposure(gate, &shutter, 1);
    std::printf("    sky exposure: street %.2f | garage entrance %.2f | deep inside %.2f | entrance with the shutter down %.2f\n",
                e_street, e_gate, e_deep, e_shut);
    check(e_street > 0.95f, "In the street the sky is open");
    check(e_gate < 0.4f && e_gate > e_deep, "At the garage entrance part of it is left");
    check(e_deep < 0.1f, "Deep inside there is none");
    check(e_shut < 0.01f, "A closed shutter shuts the rain out");
    sa::SkyProbe no_sky;
    no_sky.build(room_mesh());
    check(!no_sky.has_sky() && no_sky.exposure(deep, nullptr, 0) == 1.0f, "Maps without sky: nothing is muffled");

    // Through the engine: level and treble of the rain, outside and inside.
    const auto measure = [&](float x, bool occlusion, float volume, bool shut) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.weather_occlusion = occlusion;
        cfg.weather_volume = volume;
        auto eng = make_engine(false, cfg);
        std::string summary;
        eng->load_scene(mesh, summary);
        Camera cam;
        cam.org[0] = x;
        cam.org[2] = 64.0f;
        publish(*eng, cam);
        if (shut) {
            sa::BrushEntityState b{};
            b.entity = 9;
            b.model_index = 1;
            b.solid = true;
            b.origin[0] = 100.0f;
            eng->update_brush_entities(&b, 1);
        }
        eng->run_simulation_sync();
        Noise noise;
        feed(*eng, 0, rain, noise, 20000);
        render(*eng, 8000);
        std::vector<float> out(4800 * 2);
        eng->render_output(out.data(), 4800);
        double e = 0.0, hf = 0.0;
        for (int i = 1; i < 4800; ++i) {
            e += static_cast<double>(out[static_cast<std::size_t>(i) * 2]) * out[static_cast<std::size_t>(i) * 2];
            const double diff = out[static_cast<std::size_t>(i) * 2] - out[static_cast<std::size_t>(i - 1) * 2];
            hf += diff * diff;
        }
        return std::pair<double, double>(10.0 * std::log10(e / 4800.0), 10.0 * std::log10(hf / 4800.0));
    };
    const auto vanilla = measure(-400, false, 1.0f, false);
    const auto outside = measure(-400, true, 0.65f, false);
    const auto entrance = measure(140, true, 0.65f, false);
    const auto shut = measure(140, true, 0.65f, true);
    const auto inside = measure(700, true, 0.65f, false);
    const auto off = measure(700, false, 0.65f, false);
    std::printf("    rain: untrimmed %.1f dB | street %.1f | entrance %.1f | entrance, shutter down %.1f | deep inside %.1f (treble %.1f -> %.1f) | occlusion off %.1f\n",
                vanilla.first, outside.first, entrance.first, shut.first, inside.first, outside.second, inside.second, off.first);
    check(outside.first < vanilla.first - 3.0 && outside.first > vanilla.first - 5.0, "Outside the rain is 3 to 5 dB calmer");
    check(inside.first < outside.first - 9.0 && inside.second < outside.second - 18.0, "Under a roof it is quiet and dull");
    check(entrance.first < outside.first - 2.0 && entrance.first > inside.first + 1.0, "... half-way in between at the entrance");
    check(shut.first < entrance.first - 3.0, "... and shut out by the shutter");
    check(off.first > outside.first - 1.0, "weather_occlusion=0 is equally loud everywhere");

    // Walking in is gradual, without steps.
    sa::EngineConfig cfg{};
    cfg.sample_rate = 48000;
    auto eng = make_engine(false, cfg);
    std::string summary;
    eng->load_scene(mesh, summary);
    Camera cam;
    cam.org[0] = -400.0f;
    cam.org[2] = 64.0f;
    publish(*eng, cam);
    eng->run_simulation_sync();
    Tone tone;
    feed(*eng, 0, rain, tone, 8820);
    render(*eng, 4800);
    cam.org[0] = 700.0f; // from one frame to the next
    publish(*eng, cam);
    eng->run_simulation_sync();
    double prev = -1.0, worst = 0.0;
    for (int f = 0; f < 100; ++f) {
        feed(*eng, 0, rain, tone, 735);
        const Energy e = render(*eng, 800);
        const double db = 10.0 * std::log10(e.l + e.r);
        if (prev != -1.0) worst = std::max(worst, std::fabs(db - prev));
        prev = db;
    }
    std::printf("    largest level change per frame on entering a building %.2f dB\n", worst);
    check(worst < 1.0, "Even a teleport under a roof fades (0.4 s)");
    eng->unload_scene();
    check(eng->sky_exposure() == 1.0f, "Level shutdown forgets the roof");
}

void test_device() {
    std::printf("=== Test 23: The real output device (skipped without one) ===\n");
    sa::EngineConfig cfg{};
    cfg.reverb_enabled = false;
    sa::SteamAudioEngine eng;
    std::string err;
    if (!eng.init(cfg, /*headless=*/false, err)) {
        std::printf("    skipped: %s\n", err.c_str());
        return;
    }
    Camera cam;
    publish(eng, cam);
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // silence only
    (void)eng.get_stats();
    for (int i = 0; i < 40; ++i) {
        publish(eng, cam);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const sa::EngineStats s = eng.get_stats();
    const auto ms = [&](uint32_t frames) { return 1000.0 * frames / s.device_rate; };
    std::printf("    device %u Hz, Steam Audio %u Hz, block %u, period %u (%.1f ms), buffer %u (%.1f ms)\n", s.device_rate,
                s.output_rate, s.frame_size, s.device_period_frames, ms(s.device_period_frames), s.device_buffer_frames,
                ms(s.device_buffer_frames));
    std::printf("    queued when a period is rendered: %.1f ms (max %.1f), reserve %u, late wake-ups %u, worst block %.2f ms, blocks %llu\n",
                ms(s.queued_frames_avg), ms(s.queued_frames_max), s.reserve_frames, s.late_wakes, s.render_us_max / 1000.0,
                static_cast<unsigned long long>(s.blocks_rendered));
    check(s.device_started && s.blocks_rendered > 100, "The device runs");
    check(s.output_rate == 44100 || s.output_rate == 48000, "... at a rate Steam Audio has an HRTF for");
    check(s.frame_size >= 96 && s.frame_size <= 160, "... in small blocks");
    check(ms(s.queued_frames_avg) <= 15.0, "... and hardly any finished audio waits in WASAPI");
    const auto t0 = std::chrono::steady_clock::now();
    eng.shutdown();
    check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "It stops promptly");
}


// ---------------------------------------------------------------------------------------------------------------
// Batch 2
// ---------------------------------------------------------------------------------------------------------------

// Runs of game frames (735 dry frames each) through the engine; the output of all of it.
struct Runner {
    sa::SteamAudioEngine& eng;
    sa::ChannelDescriptor d;
    Tone tone;
    std::vector<float> run(int game_frames, bool feeding = true) {
        std::vector<float> all;
        const int n = eng.output_rate() / 60;
        for (int f = 0; f < game_frames; ++f) {
            if (feeding) feed(eng, 0, d, tone, 735);
            else feed_silence(eng, 735); // the engine paints on when nothing plays
            const std::size_t at = all.size();
            all.resize(at + static_cast<std::size_t>(n) * 2);
            eng.render_output(all.data() + at, static_cast<uint32_t>(n));
        }
        return all;
    }
};

float peak_of(const std::vector<float>& s, std::size_t from = 0, std::size_t to = static_cast<std::size_t>(-1)) {
    float p = 0.0f;
    for (std::size_t i = from; i < std::min(to, s.size()); ++i) p = std::max(p, std::fabs(s[i]));
    return p;
}

float max_step_of(const std::vector<float>& s) {
    float m = 0.0f;
    for (std::size_t i = 2; i < s.size(); ++i) m = std::max(m, std::fabs(s[i] - s[i - 2]));
    return m;
}

void test_focus() {
    std::printf("=== Test 24: Alt-Tab, pause and the engine stopping its device ===\n");
    const sa::AcousticMeshData mesh = room_mesh();
    std::string summary;
    Camera cam;
    for (int mode = 0; mode < 2; ++mode) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.reverb_mode = mode == 1 ? sa::ReverbMode::kHybrid : sa::ReverbMode::kParametric;
        cfg.reverb_wet = 2.0f;
        auto eng = make_engine(true, cfg);
        eng->load_scene(mesh, summary);
        publish(*eng, cam);
        run_ticks(*eng, 3);
        Runner r{ *eng, source_at(200.0f, 0.0f, 0.0f, 0.005f), Tone{} };
        r.run(20);
        const std::vector<float> before = r.run(20);
        const float loud = peak_of(before), step = max_step_of(before);

        eng->set_focused(false);
        const std::vector<float> away = r.run(6); // 100 ms
        check(max_step_of(away) <= step * 1.1f + 1e-4f, "Alt-Tab: fades, no click");
        check(peak_of(away, 0, 800) > loud * 0.5f && peak_of(away, away.size() * 7 / 10) == 0.0f, "... within 70 ms");
        const std::vector<float> silent = r.run(120);
        check(peak_of(silent) == 0.0f, "... to digital silence, reverb included, while the game goes on");
        check(eng->get_stats().silent && !eng->get_stats().focused, "sa_status says so");
        check(eng->buffered_frames() < 4000, "Nothing piles up meanwhile");
        // Nothing plays any more when the player comes back: a tail would be 2 s old.
        eng->notify_channel_freed(0);
        r.run(10, false);
        eng->set_focused(true);
        const std::vector<float> nothing = r.run(20, false);
        check(peak_of(nothing) < 1e-6f, "Coming back plays nothing stale and no old reverb");
        Runner again{ *eng, r.d, Tone{} };
        again.d.mixer = 77;
        again.run(10);
        eng->set_focused(false);
        again.run(30);
        eng->set_focused(true);
        const std::vector<float> back = again.run(8);
        check(max_step_of(back) <= step * 1.4f && peak_of(back, 0, 400) < loud * 0.15f, "Coming back fades in, no click");
        const std::vector<float> full = again.run(20);
        check(peak_of(full) > loud * 0.8f && peak_of(full) < loud * 1.25f, "... to the old level");

        eng->set_mute_in_background(false);
        eng->set_focused(false);
        again.run(10);
        check(peak_of(again.run(10)) > loud * 0.8f, "mute_in_background=0 plays on in the background");
        eng->set_mute_in_background(true);
        again.run(10);
        check(peak_of(again.run(4)) == 0.0f, "... and takes effect at once when it is set");
        eng->set_focused(true);
        again.run(20);

        // The pause menu: the engine stops feeding world sounds. The room must not ring on for seconds, or burst when
        // the game goes on.
        eng->set_game_paused(true);
        eng->notify_channel_freed(0);
        feed_silence(*eng, 735 * 80);
        std::vector<float> paused(48000 * 2);
        eng->render_output(paused.data(), 48000);
        check(peak_of(paused, 2 * 24000) == 0.0f, "A paused game is silent after half a second, reverb included");
        check(max_step_of(paused) <= step * 1.1f + 1e-3f, "... without a click");
        eng->set_game_paused(false);
        feed_silence(*eng, 735 * 20);
        std::vector<float> resumed(9600 * 2);
        eng->render_output(resumed.data(), 9600);
        check(peak_of(resumed) < 1e-6f, "... and nothing old is heard when it goes on");
        Runner third{ *eng, r.d, Tone{} };
        third.d.mixer = 78;
        third.run(30);
        eng->notify_channel_freed(0);
        feed_silence(*eng, 735 * 10);
        render(*eng, 48000 / 10);
        float tail = 0.0f;
        render(*eng, 2400, &tail);
        check(tail > 1e-5f, "... but the room answers again");
        // Menu sounds play while the game is paused.
        eng->set_game_paused(true);
        sa::ChannelDescriptor ui{};
        ui.active = true;
        ui.mixer = 5;
        Runner menu{ *eng, ui, Tone{} };
        menu.run(40);
        check(peak_of(menu.run(10)) > 0.1f, "Menu sounds play while the game is paused");
    }

    // S_Pause: the clock stands still.
    sa::EngineConfig cfg{};
    auto eng = make_engine(false, cfg);
    publish(*eng, cam);
    sa::ChannelDescriptor ui{};
    ui.active = true;
    ui.mixer = 5;
    Tone tone;
    feed(*eng, 0, ui, tone, 8820);
    std::vector<float> a(2048 * 2);
    eng->render_output(a.data(), 2048);
    const float step = max_step_of(a) * 1.1f + 1e-3f;
    eng->set_held(true);
    std::vector<float> b(2048 * 2);
    eng->render_output(b.data(), 2048);
    const uint32_t held_at = eng->buffered_frames();
    check(max_step_of(b) < step && peak_of(b, 512 * 2) == 0.0f, "Stopped device: one block fades out, then silence");
    render(*eng, 44100);
    check(eng->buffered_frames() == held_at && held_at > 4000, "... and what is buffered waits");
    check(eng->get_stats().held, "sa_status says so");
    const uint32_t starved = eng->get_stats().starvations;
    eng->set_held(false);
    std::vector<float> c(2048 * 2);
    eng->render_output(c.data(), 2048);
    check(max_step_of(c) < step && peak_of(c) > 0.1f, "It goes on from there with a fade");
    check(eng->get_stats().starvations == starved, "... which is no dropout");
    eng->set_held(true);
    render(*eng, 1024);
    eng->flush_audio();
    render(*eng, 1024);
    check(eng->buffered_frames() == 0 && eng->voices_in_use() == 0, "A flush (load game) while it is stopped empties it");
    eng->set_held(false);
    float peak = 0.0f;
    render(*eng, 4096, &peak);
    check(peak < 1e-5f, "... and nothing of that is heard afterwards");
    feed(*eng, 0, ui, tone, 4410);
    peak = 0.0f;
    render(*eng, 2048, &peak);
    check(peak > 0.1f, "New sounds are");
}

void test_pacer() {
    std::printf("=== Test 25: Output pacing follows the device ===\n");
    sa::OutputPacer p;
    check(p.configure(1056, 480, 48000), "A first device is set up");
    const uint32_t reserve = p.reserve();
    check(reserve >= 192 && reserve <= 576, "Reserve: one audio engine pass, never more than buffer - period");
    check(!p.configure(1056, 480, 48000), "The same device changes nothing");
    uint32_t t = 1000;
    p.woke(t, 100);
    for (int i = 0; i < 3; ++i) {
        t += 200000; // far too late
        check(p.woke(t, 0), "A dropout is noticed");
    }
    const uint32_t grown = p.reserve();
    check(grown > reserve && grown <= 576, "Three in five seconds: one more period in reserve");
    check(p.queued_average() > 0.0f, "It learned something about this device");
    check(p.configure(1056, 480, 44100), "Another rate with the same buffer is another device");
    check(p.reserve() != grown && p.queued_average() == 0.0f, "... and starts anew");
    check(!p.woke(t + 900000, 0), "... so the pause while it was opened is no dropout");
    check(p.configure(1056, 128, 44100), "Another period with the same buffer, too");
    check(p.reserve() >= 127 && p.reserve() <= 1056 - 128, "... with a reserve that fits it");
    check(p.configure(2000, 128, 44100), "Another buffer, too");
    p.reset();
    check(p.configure(2000, 128, 44100), "And the same numbers after a device was closed");
    check(p.configure(480, 480, 48000) && p.reserve() == 0, "A buffer of one period leaves no room for a reserve");
    check(!p.woke(5, 0) && !p.woke(10, 0), "... and works");
    // The wrap of the microsecond clock (every 71 minutes).
    p.configure(1056, 480, 48000);
    p.woke(0xFFFFF000u, 10);
    check(!p.woke(0x00000500u, 0), "The clock may wrap around");
}

void test_device_change() {
    std::printf("=== Test 26: Another output device, reverb mode or HRTF while playing ===\n");
    const sa::AcousticMeshData mesh = room_mesh();
    std::string summary, err;
    Camera cam;
    sa::EngineConfig cfg{};
    cfg.sample_rate = 48000;
    auto eng = make_engine(true, cfg);
    eng->load_scene(mesh, summary);
    publish(*eng, cam);
    run_ticks(*eng, 3);
    Runner r{ *eng, source_at(200.0f, 0.0f, 0.0f, 0.005f), Tone{} };
    r.run(20);
    const float loud = peak_of(r.run(20));
    const auto plays = [&](const char* what) {
        run_ticks(*eng, 1);
        const std::vector<float> first = r.run(10);
        run_ticks(*eng, 3);
        const std::vector<float> s = r.run(30);
        check(std::isfinite(peak_of(s)) && peak_of(s) > loud * 0.75f && peak_of(s) < loud * 1.35f, what);
        check(max_step_of(first) < 0.05f, "... starting with a fade");
    };
    const auto pitch = [&](const std::vector<float>& s) { // zero crossings of the left channel
        int n = 0;
        for (std::size_t i = 2; i < s.size(); i += 2) n += (s[i] >= 0.0f) != (s[i - 2] >= 0.0f) ? 1 : 0;
        return 0.5 * n * eng->output_rate() / (s.size() / 2);
    };

    check(eng->change_output_sync(44100, 441, err), "48 -> 44.1 kHz");
    check(eng->output_rate() == 44100 && eng->get_stats().dsp_rebuilds == 1, "... rebuilds Steam Audio");
    plays("It plays at the same level");
    {
        const std::vector<float> s = r.run(60);
        check(std::fabs(pitch(s) - 440.0) < 5.0, "... and pitch");
    }
    check(eng->get_stats().reverb_time[0] > 0.1f, "... with the room's reverb");
    check(eng->change_output_sync(48000, 128, err) && eng->output_rate() == 48000, "44.1 -> 48 kHz with a 128 frame period");
    check(eng->frame_size() == 128, "... in blocks that fit the period");
    plays("It plays");
    {
        const std::vector<float> s = r.run(60);
        check(std::fabs(pitch(s) - 440.0) < 5.0, "... at the right pitch");
    }
    const uint32_t rebuilds = eng->get_stats().dsp_rebuilds;
    check(eng->change_output_sync(48000, 128, err) && eng->get_stats().dsp_rebuilds == rebuilds, "The same format needs no rebuild");
    plays("It plays");

    // No device for a while.
    feed(*eng, 0, r.d, r.tone, 60000);
    check(eng->get_stats().dropped_chunks > 0, "Without a device the timeline fills up, then drops");
    check(eng->change_output_sync(48000, 128, err), "It comes back");
    check(eng->buffered_frames() == 0, "... and what was queued long ago is not played");
    plays("It plays");

    // Reverb mode.
    check(eng->get_stats().reverb_mode == sa::ReverbMode::kHybrid, "Hybrid");
    eng->request_reverb_mode(sa::ReverbMode::kParametric);
    check(eng->get_stats().reverb_mode == sa::ReverbMode::kParametric && eng->config().reverb_mode == sa::ReverbMode::kParametric,
          "sa_reverb_mode parametric");
    std::string msg;
    check(eng->pop_message(msg) && msg.find("parametric") != std::string::npos, "... says so");
    plays("It plays");
    check(eng->get_stats().reverb_time[0] > 0.1f, "... with reverb");
    eng->request_reverb_mode(sa::ReverbMode::kHybrid);
    check(eng->get_stats().reverb_mode == sa::ReverbMode::kHybrid, "sa_reverb_mode hybrid");
    plays("It plays");
    const uint32_t count = eng->get_stats().dsp_rebuilds;
    while (eng->pop_message(msg)) {}
    eng->request_reverb_mode(sa::ReverbMode::kHybrid);
    check(eng->get_stats().dsp_rebuilds == count && eng->pop_message(msg), "The mode it has already interrupts nothing, and says so");

    // Doors keep working after a rebuild: the scene stays.
    check(eng->get_stats().scene_triangles > 0 && eng->get_stats().loaded_map == "room", "The level's geometry stays loaded");

    // HRTF files.
    check(eng->get_stats().hrtf_name == "default" && eng->get_stats().hrtf_warning.empty(), "The built-in HRTF");
    eng->request_hrtf("missing.sofa", std::wstring());
    check(eng->get_stats().dsp_rebuilds == count && eng->config().sofa_file.empty(), "sa_sofa with a missing file changes nothing");
    check(eng->pop_message(msg) && msg.find("missing.sofa") != std::string::npos, "... and says why");
    plays("It plays");
}

struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"auspex_audio_test_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(path.c_str(), nullptr);
        CreateDirectoryW((path + L"\\sofa").c_str(), nullptr);
    }
    ~TempDir() {
        for (const wchar_t* sub : { L"\\sofa", L"" }) {
            WIN32_FIND_DATAW data{};
            const HANDLE h = FindFirstFileW((path + sub + L"\\*").c_str(), &data);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        SetFileAttributesW((path + sub + L"\\" + data.cFileName).c_str(), FILE_ATTRIBUTE_NORMAL);
                        DeleteFileW((path + sub + L"\\" + data.cFileName).c_str());
                    }
                } while (FindNextFileW(h, &data));
                FindClose(h);
            }
            RemoveDirectoryW((path + sub).c_str());
        }
    }
    void write(const wchar_t* name, const std::string& text) {
        std::string err;
        sa::write_file_atomic(path + L"\\" + name, text, err);
    }
};

void test_sofa() {
    std::printf("=== Test 27: Custom HRTF files ===\n");
    TempDir dir;
    std::string garbage(4096, '\0');
    for (std::size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<char>(i * 37 + 11);
    dir.write(L"top.sofa", garbage);
    dir.write(L"sofa\\sub.sofa", garbage);
    dir.write(L"both.sofa", garbage);
    dir.write(L"sofa\\both.sofa", garbage);
    dir.write(L"sofa\\empty.sofa", std::string());
    dir.write(L"sofa\\with space \u00e9\u4e2d.sofa", garbage);
    dir.write(L"sofa\\other.txt", garbage);
    std::wstring p;
    check(sa::resolve_sofa(dir.path, "top.sofa", p) && p == dir.path + L"\\top.sofa", "In the mod folder");
    check(sa::resolve_sofa(dir.path, "sub.sofa", p) && p == dir.path + L"\\sofa\\sub.sofa", "In its folder sofa");
    check(sa::resolve_sofa(dir.path, "both.sofa", p) && p == dir.path + L"\\both.sofa", "The mod folder comes first");
    check(sa::resolve_sofa(dir.path, "sub", p) && p == dir.path + L"\\sofa\\sub.sofa", "\".sofa\" may be left out");
    check(sa::resolve_sofa(dir.path, "sofa/sub.sofa", p) && !p.empty(), "A relative path with folders");
    check(sa::resolve_sofa(std::wstring(), sa::narrow(dir.path + L"\\top.sofa"), p) && p == dir.path + L"\\top.sofa", "An absolute path");
    check(sa::resolve_sofa(dir.path, "\"top.sofa\"", p), "In quotes");
    check(sa::resolve_sofa(dir.path, sa::narrow(L"with space \u00e9\u4e2d.sofa"), p), "Spaces and letters of any language");
    check(!sa::resolve_sofa(dir.path, "nothing.sofa", p) && p.empty(), "A file that is not there");
    check(!sa::resolve_sofa(dir.path, "", p) && !sa::resolve_sofa(std::wstring(), "top.sofa", p), "No file, no folder");
    check(!sa::resolve_sofa(dir.path, "sofa", p), "A folder is no file");
    const std::vector<std::string> files = sa::list_sofa_files(dir.path);
    check(files.size() == 6, "sa_sofa lists the .sofa files");
    check(sa::folder_above(L"C:\\g\\mod\\blse\\plugins\\x.dll", 2) == L"C:\\g\\mod", "The mod folder is two above the plugin's");
    check(sa::folder_above(L"C:\\x.dll", 2).empty(), "... if there is one");

    Camera cam;
    const auto start_with = [&](const char* file) {
        sa::EngineConfig cfg{};
        cfg.sofa_file = file;
        sa::resolve_sofa(dir.path, cfg.sofa_file, cfg.sofa_path);
        return make_engine(false, cfg);
    };
    const auto audible = [&](sa::SteamAudioEngine& eng) {
        publish(eng, cam);
        Runner r{ eng, source_at(100.0f, 100.0f, 0.0f, 0.005f), Tone{} };
        r.run(10);
        return peak_of(r.run(10)) > 0.02f;
    };
    {
        auto eng = start_with("nothing.sofa");
        const sa::EngineStats s = eng->get_stats();
        check(s.hrtf_name == "default" && s.hrtf_warning.find("not found") != std::string::npos, "A missing file: built-in HRTF and a warning");
        check(audible(*eng) && eng->config().sofa_file == "nothing.sofa", "... sound plays, and the setting is kept for sa_save");
    }
    {
        auto eng = start_with("top.sofa");
        const sa::EngineStats s = eng->get_stats();
        check(s.hrtf_name == "default" && s.hrtf_warning.find("top.sofa") != std::string::npos, "A file that is no SOFA: built-in HRTF and a warning");
        check(audible(*eng), "... sound plays");
        const uint32_t rebuilds = eng->get_stats().dsp_rebuilds;
        std::string msg;
        sa::resolve_sofa(dir.path, "sub.sofa", p);
        eng->request_hrtf("sub.sofa", p);
        check(eng->get_stats().dsp_rebuilds == rebuilds && eng->pop_message(msg) && msg.find("sub.sofa") != std::string::npos,
              "sa_sofa with such a file changes nothing and says why");
        eng->request_hrtf(std::string(), std::wstring());
        check(eng->get_stats().hrtf_warning.empty() && eng->config().sofa_file.empty() && eng->pop_message(msg) &&
              msg.find("default") != std::string::npos, "sa_sofa default");
        check(audible(*eng), "... sound plays");
    }
    {
        auto eng = start_with("empty.sofa");
        check(eng->get_stats().hrtf_name == "default" && !eng->get_stats().hrtf_warning.empty() && audible(*eng), "An empty file");
    }
    // A real one, if the caller has one: offline_tests.exe --sofa <file>
    if (!g_sofa_file.empty()) {
        sa::EngineConfig cfg{};
        cfg.sofa_file = sa::narrow(g_sofa_file);
        cfg.sofa_path = g_sofa_file;
        auto eng = make_engine(false, cfg);
        const sa::EngineStats s = eng->get_stats();
        std::printf("    HRTF '%s' %s\n", s.hrtf_name.c_str(), s.hrtf_warning.c_str());
        check(s.hrtf_name != "default" && s.hrtf_warning.empty() && audible(*eng), "A real .sofa file loads and plays");
        std::string msg;
        eng->request_hrtf(std::string(), std::wstring());
        check(eng->get_stats().hrtf_name == "default" && audible(*eng), "... back to the built-in one");
        eng->request_hrtf(cfg.sofa_file, cfg.sofa_path);
        check(eng->get_stats().hrtf_name != "default" && audible(*eng), "... and to it again while playing");
        check(eng->change_output_sync(48000, 480, msg) && eng->get_stats().hrtf_name != "default" && audible(*eng),
              "... and it stays when the output rate changes");
    }
}

void test_directivity() {
    std::printf("=== Test 28: Voices have a direction ===\n");
    // Angles.
    const auto fwd = [](float pitch, float yaw) {
        const float a[3] = { pitch, yaw, 30.0f };
        std::vector<float> f(3);
        sa::source_angles_forward(a, f.data());
        return f;
    };
    const auto is = [](const std::vector<float>& f, float x, float y, float z) {
        return nearf(f[0], x, 1e-5f) && nearf(f[1], y, 1e-5f) && nearf(f[2], z, 1e-5f);
    };
    check(is(fwd(0, 0), 1, 0, 0) && is(fwd(0, 90), 0, 1, 0) && is(fwd(0, 180), -1, 0, 0) && is(fwd(0, -90), 0, -1, 0), "Yaw");
    check(is(fwd(90, 0), 0, 0, -1) && is(fwd(-90, 0), 0, 0, 1), "Pitch: positive looks down");
    check(is(fwd(0, 450), 0, 1, 0), "Any number of turns");
    const float north[3] = { 0, 1, 0 };
    const IPLVector3 ipl = sa::source_dir_to_ipl(north);
    check(nearf(ipl.x, -1.0f) && nearf(ipl.y, 0.0f) && nearf(ipl.z, 0.0f), "Source +Y (left) is Steam Audio -X");

    // The model against Steam Audio's own dipole.
    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.simdLevel = IPL_SIMDLEVEL_SSE2;
    IPLContext ctx = nullptr;
    iplContextCreate(&cs, &ctx);
    float worst = 0.0f;
    for (int deg = 0; deg <= 360; deg += 5) {
        const float a = deg * kPi / 180.0f;
        IPLCoordinateSpace3 space{};
        space.right = { 1, 0, 0 };
        space.up = { 0, 1, 0 };
        space.ahead = { 0, 0, -1 };
        IPLDirectivity dip{};
        dip.dipoleWeight = sa::kVoiceDipoleWeight;
        dip.dipolePower = sa::kVoiceDipolePower;
        const IPLVector3 listener{ 3.0f * std::sin(a), 0.0f, -3.0f * std::cos(a) };
        worst = std::max(worst, std::fabs(iplDirectivityCalculate(ctx, space, listener, &dip) - sa::dipole_directivity(std::cos(a))));
    }
    iplContextRelease(&ctx);
    std::printf("    largest difference to iplDirectivityCalculate %.5f\n", worst);
    check(worst < 1e-3f, "The dipole is Steam Audio's");
    const IPLVector3 at{ 0, 0, 0 }, ahead{ 0, 0, -1 };
    const auto dir = [&](float deg, float dist) {
        const float a = deg * kPi / 180.0f;
        return sa::voice_directivity(ahead, at, IPLVector3{ dist * std::sin(a), 0.0f, -dist * std::cos(a) });
    };
    const auto db = [](float g) { return 20.0f * std::log10(g); };
    const sa::VoiceDirectivity front = dir(0, 3), side = dir(90, 3), behind = dir(180, 3);
    std::printf("    behind: %.1f dB, middle %.1f dB, high %.1f dB; beside: %.1f dB\n", db(behind.gain), db(behind.gain * behind.mid),
                db(behind.gain * behind.high), db(side.gain));
    check(nearf(front.gain, 1.0f) && nearf(front.mid, 1.0f) && nearf(front.high, 1.0f), "From the front: unchanged");
    check(db(behind.gain) < -2.5f && db(behind.gain) > -4.0f, "From behind: 3 to 4 dB softer");
    check(db(behind.gain * behind.high) < -5.0f && db(behind.gain * behind.high) > -7.0f, "... and 6 dB in the treble");
    check(side.gain < front.gain && side.gain > behind.gain, "Beside: in between");
    bool monotonic = true;
    for (int deg = 0; deg < 180; ++deg) {
        if (dir(static_cast<float>(deg + 1), 3).gain > dir(static_cast<float>(deg), 3).gain + 1e-6f) monotonic = false;
    }
    check(monotonic, "Smaller and smaller on the way around");
    check(nearf(dir(180, 0.3f).gain, 1.0f) && nearf(dir(180, 0.0f).gain, 1.0f), "No direction right at the mouth");
    check(std::fabs(dir(180, 0.99f).gain - dir(180, 1.01f).gain) < 0.01f, "... and no step towards that");

    // What a channel says.
    sa::ChannelSnapshot c{};
    c.name = "character/dlg/bum/bark.mp3";
    c.entnum = 40;
    c.entchannel = sa::kChanVoice;
    c.dist_mult = dist_mult_for(80, 36);
    c.master_vol = 255;
    c.is_voice = true;
    c.distance = 100.0f;
    c.mouth_height = 64.0f;
    c.has_facing = true;
    c.facing[0] = 1.0f;
    check(sa::describe_channel(c).directional && nearf(sa::describe_channel(c).forward[0], 1.0f), "A character's bark has a direction");
    sa::ChannelSnapshot x = c;
    x.entchannel = sa::kChanStream;
    x.skip_collide = true;
    check(sa::describe_channel(x).dialogue && !sa::describe_channel(x).directional, "A conversation line has none");
    x = c; x.is_voice = false;
    check(!sa::describe_channel(x).directional, "An effect has none");
    x = c; x.mouth_height = 0.0f;
    check(!sa::describe_channel(x).directional, "A voice that comes from no body (radio, speaker) has none");
    x = c; x.has_facing = false;
    check(!sa::describe_channel(x).directional, "Nor one whose angles are unknown");
    x = c; x.facing_recorded = true;
    check(!sa::describe_channel(x).directional, "Nor a file recorded from both sides ('<'): the engine blends those");
    x = c; x.own = true; x.entnum = 1;
    check(!sa::describe_channel(x).directional, "Nor the player");
    x = c; x.dist_mult = 0.0f;
    check(!sa::describe_channel(x).directional, "Nor what plays everywhere");
    x = c; x.facing[0] = std::nanf("");
    check(!sa::describe_channel(x).directional, "Nor one with broken angles");

    // Heard.
    Camera cam;
    const auto level = [&](float facing_x, bool directional, bool on, double* treble) {
        sa::EngineConfig cfg{};
        cfg.voice_directivity = on;
        auto eng = make_engine(false, cfg);
        publish(*eng, cam);
        sa::ChannelDescriptor d = source_at(120.0f, 0.0f, 0.0f, 0.005f);
        d.is_voice = true;
        d.directional = directional;
        d.forward[0] = facing_x;
        Noise noise;
        feed(*eng, 0, d, noise, 22050);
        render(*eng, 4410);
        std::vector<float> out(8820 * 2);
        eng->render_output(out.data(), 8820);
        double all = 0, high = 0;
        for (std::size_t i = 4; i < out.size(); i += 2) {
            const double v = out[i], h = out[i] - 2.0 * out[i - 2] + out[i - 4];
            all += v * v;
            high += h * h;
        }
        if (treble) *treble = high;
        const std::vector<sa::VoiceInfo> infos = eng->voice_infos();
        if (directional && on) {
            check(infos.size() == 1 && infos[0].directional && (facing_x < 0 ? infos[0].directivity > 0.999f : infos[0].directivity < 0.75f),
                  "sa_debug shows it");
        }
        return all;
    };
    double high_to = 0, high_away = 0, high_plain = 0, high_off = 0;
    const double to = level(-1.0f, true, true, &high_to), away = level(1.0f, true, true, &high_away);
    const double plain = level(1.0f, false, true, &high_plain), off = level(1.0f, true, false, &high_off);
    const double drop = 10.0 * std::log10(away / to), drop_high = 10.0 * std::log10(high_away / high_to);
    std::printf("    facing away: %.2f dB, treble %.2f dB\n", drop, drop_high);
    check(drop < -2.5 && drop > -7.0 && drop_high < drop - 0.3, "Facing away is heard: softer, and duller");
    check(std::fabs(10.0 * std::log10(plain / to)) < 0.05, "A conversation line is bit for bit as loud as one to the face");
    check(std::fabs(10.0 * std::log10(off / to)) < 0.05, "sa_directivity 0 turns it off");

    // A character that turns around in one frame.
    auto eng = make_engine(false);
    publish(*eng, cam);
    sa::ChannelDescriptor d = source_at(120.0f, 0.0f, 0.0f, 0.005f);
    d.is_voice = true;
    d.directional = true;
    d.forward[0] = -1.0f;
    Runner r{ *eng, d, Tone{} };
    r.run(20);
    const float step = max_step_of(r.run(10));
    r.d.forward[0] = 1.0f;
    const std::vector<float> turn = r.run(30);
    check(max_step_of(turn) < step * 1.15f, "Turning around is smooth");
    check(peak_of(turn, turn.size() * 3 / 4) < peak_of(turn, 0, turn.size() / 30) * 0.8f, "... and complete within half a second");
}

void test_settings() {
    std::printf("=== Test 29: Presets and the settings file ===\n");
    std::size_t count = 0;
    const sa::Preset* all = sa::presets(count);
    check(count == 4 && sa::find_preset("headphones") && sa::find_preset("SPEAKERS") && sa::find_preset("subtle") &&
          sa::find_preset("Cinematic"), "headphones, speakers, subtle, cinematic");
    check(!sa::find_preset("nope") && !sa::find_preset(nullptr), "Other names are not presets");
    const sa::EngineConfig def{};
    check(sa::matching_preset(def) == sa::find_preset("headphones"), "The defaults are the preset for headphones");
    for (std::size_t i = 0; i < count; ++i) {
        sa::EngineConfig cfg{};
        cfg.enabled = false;
        cfg.master_volume = 0.7f;
        cfg.weather_volume = 1.1f;
        cfg.sofa_file = "mine.sofa";
        cfg.sample_rate = 44100;
        cfg.reverb_mode = sa::ReverbMode::kParametric;
        sa::apply_preset(all[i], cfg);
        check(sa::matching_preset(cfg) == &all[i] && all[i].description[0] != '\0', all[i].name);
        check(!cfg.enabled && cfg.master_volume == 0.7f && cfg.weather_volume == 1.1f && cfg.sofa_file == "mine.sofa" &&
              cfg.sample_rate == 44100 && cfg.reverb_mode == sa::ReverbMode::kParametric, "... leaves volumes, HRTF file and quality alone");
        sa::EngineConfig again = cfg;
        again.reverb_wet += 0.1f;
        check(sa::matching_preset(again) != &all[i], "... and a changed setting is no longer that preset");
    }
    check(!sa::find_preset("speakers")->hrtf && sa::find_preset("headphones")->hrtf, "HRTF is for headphones");
    check(sa::find_preset("subtle")->reverb_wet < sa::find_preset("headphones")->reverb_wet &&
          sa::find_preset("headphones")->reverb_wet < sa::find_preset("cinematic")->reverb_wet, "From little to much reverb");
    {
        auto eng = make_engine(false);
        sa::EngineConfig cfg = eng->config();
        sa::apply_preset(*sa::find_preset("speakers"), cfg);
        eng->apply_settings(cfg);
        check(sa::matching_preset(eng->config()) == sa::find_preset("speakers"), "Presets apply to the running engine at once");
        eng->set_weather_volume(9.0f);
        check(eng->config().weather_volume == 2.0f, "sa_weather_vol stays within 0 to 2");
        eng->set_weather_volume(0.4f);
        eng->set_enabled(false);
        check(eng->config().weather_volume == 0.4f && !eng->config().enabled, "Every command changes what sa_save saves");
        eng->set_voice_directivity(false);
        eng->set_mute_in_background(false);
        check(!eng->config().voice_directivity && !eng->config().mute_in_background, "... these too");
        eng->set_rain_masking(7.0f);
        check(eng->config().rain_masking == 2.0f, "sa_rain stays within 0 to 2");
        sa::EngineConfig tweaked = eng->config();
        tweaked.dialogue_glass = tweaked.fix_npc_barks = tweaked.level_voices = false;
        tweaked.tame_loud_sounds = tweaked.fix_ambient_directions = false;
        tweaked.filter_heels_ambients = tweaked.filter_hoof_ambient = false;
        eng->set_tweaks(tweaked);
        const sa::ChannelTweaks t = eng->channel_tweaks();
        check(!eng->config().dialogue_glass && !t.npc_barks && !t.level_voices && !t.tame_loud && !t.ambient_directions &&
              !t.no_heels && !t.no_hoofs && !eng->config().filter_heels_ambients && !eng->config().filter_hoof_ambient,
              "sa_tweaks reaches every correction");
    }

    // Round trip.
    sa::EngineConfig odd{};
    odd.enabled = false;
    odd.master_volume = 1.35f;
    odd.mute_in_background = false;
    odd.hrtf_enabled = false;
    odd.sofa_file = "my ears; left = right #1.sofa";
    odd.occlusion_enabled = false;
    odd.occlusion_samples = 33;
    odd.dynamic_doors = false;
    odd.voice_directivity = false;
    odd.reverb_enabled = false;
    odd.reverb_mode = sa::ReverbMode::kParametric;
    odd.early_reflections = 0.45f;
    odd.reverb_wet = 0.123f;
    odd.reflection_rays = 2048;
    odd.reflection_bounces = 7;
    odd.reverb_duration = 2.25f;
    odd.weather_occlusion = false;
    odd.weather_volume = 1.75f;
    odd.rain_masking = 0.35f;
    odd.dialogue_glass = false;
    odd.fix_npc_barks = false;
    odd.level_voices = false;
    odd.tame_loud_sounds = false;
    odd.fix_ambient_directions = false;
    odd.filter_heels_ambients = false;
    odd.filter_hoof_ambient = false;
    odd.sample_rate = 48000;
    const auto parse = [](const std::string& text) {
        return sa::read_config([&](const char* s, const char* k, std::string& v) { return sa::ini_lookup(text, s, k, v); });
    };
    const auto same = [](const sa::EngineConfig& a, const sa::EngineConfig& b) {
        return a.enabled == b.enabled && a.master_volume == b.master_volume && a.mute_in_background == b.mute_in_background &&
               a.hrtf_enabled == b.hrtf_enabled && a.sofa_file == b.sofa_file && a.occlusion_enabled == b.occlusion_enabled &&
               a.occlusion_samples == b.occlusion_samples && a.dynamic_doors == b.dynamic_doors &&
               a.voice_directivity == b.voice_directivity && a.reverb_enabled == b.reverb_enabled &&
               a.reverb_mode == b.reverb_mode && a.early_reflections == b.early_reflections && a.reverb_wet == b.reverb_wet &&
               a.reflection_rays == b.reflection_rays && a.reflection_bounces == b.reflection_bounces &&
               a.reverb_duration == b.reverb_duration && a.weather_occlusion == b.weather_occlusion &&
               a.weather_volume == b.weather_volume && a.sample_rate == b.sample_rate &&
               a.rain_masking == b.rain_masking && a.dialogue_glass == b.dialogue_glass && a.fix_npc_barks == b.fix_npc_barks &&
               a.level_voices == b.level_voices && a.tame_loud_sounds == b.tame_loud_sounds &&
               a.fix_ambient_directions == b.fix_ambient_directions && a.filter_heels_ambients == b.filter_heels_ambients &&
               a.filter_hoof_ambient == b.filter_hoof_ambient;
    };
    check(same(parse(sa::format_ini(odd)), odd), "Every setting survives the file");
    check(same(parse(sa::format_ini(def)), def), "... the defaults, too");
    check(same(parse(""), def) && same(parse("[Spatial]\nhrtf=\n"), def), "Missing and empty keys are the defaults");
    const std::string text = sa::format_ini(odd);
    for (const char* key : { "[General]", "[Spatial]", "[Weather]", "[Tweaks]", "[Output]", "\r\nenabled=0", "\r\nmaster_volume=", "\r\nmute_in_background=",
                             "\r\nhrtf=", "\r\nsofa_file=", "\r\nocclusion=", "\r\nocclusion_samples=", "\r\ndynamic_doors=",
                             "\r\nvoice_directivity=0", "\r\nreverb=", "\r\nreverb_mode=parametric", "\r\nearly_reflections=",
                             "\r\nreverb_wet=", "\r\nreflection_rays=", "\r\nreflection_bounces=", "\r\nreverb_duration=",
                             "\r\nweather_occlusion=", "\r\nweather_volume=", "\r\nrain_masking=", "\r\ndialogue_glass=0",
                             "\r\nfix_npc_barks=0", "\r\nlevel_voices=0", "\r\ntame_loud_sounds=0",
                             "\r\nfix_ambient_directions=0", "\r\nfilter_heels_ambients=0", "\r\nfilter_hoof_ambient=0", "\r\nsample_rate=48000" }) {
        if (text.find(key) == std::string::npos) check(false, key);
    }
    {
        bool commented = true, crlf = true;
        std::string prev;
        for (std::size_t at = 0; at < text.size();) {
            const std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos || nl == at || text[nl - 1] != '\r') { crlf = false; break; }
            const std::string line = text.substr(at, nl - 1 - at);
            if (line.find('=') != std::string::npos && line[0] != ';' && prev[0] != ';') commented = false;
            prev = line.empty() ? " " : line;
            at = nl + 1;
        }
        check(crlf && commented, "All sections and keys are there, every key is explained");
    }
    sa::EngineConfig wild = parse("[general]\nMASTER_VOLUME = 99\n[spatial]\nocclusion_samples=1\nreverb_wet=nan\nreflection_rays=9999999\n"
                                  "reflection_bounces=-4\nreverb_duration=100\nearly_reflections=0\nsofa_file=Default\n"
                                  "[weather]\nweather_volume=-3\nrain_masking=77\n[output]\nsample_rate=96000\n; [x]\n# y=1\n");
    check(wild.master_volume == 2.0f && wild.occlusion_samples == 4 && wild.reverb_wet == def.reverb_wet && wild.reflection_rays == 4096 &&
          wild.reflection_bounces == 1 && wild.reverb_duration == 3.0f && wild.early_reflections == 0.1f &&
          wild.weather_volume == 0.0f && wild.rain_masking == 2.0f && wild.sample_rate == 0 && wild.sofa_file.empty(), "Anything out of range is put right");
    const std::string kept = sa::format_ini(odd, "\xEF\xBB\xBF[General]\nfuture = 7\nenabled=1\n[New]\nthing=a=b\n; note\n[Spatial]\nx=\n");
    std::string v;
    check(sa::ini_lookup(kept, "General", "future", v) && v == "7" && sa::ini_lookup(kept, "New", "thing", v) && v == "a=b" &&
          sa::ini_lookup(kept, "Spatial", "x", v) && v.empty() && same(parse(kept), odd), "Keys of another version are kept");
    check(sa::format_ini(odd, kept) == kept, "Saving twice changes nothing more");
    {
        // The file of the version before.
        const std::string old = "[Tweaks]\nfix_npc_barks=1\nsoften_loud_barks=0\nfilter_odd_ambients=0\nmine=1\n";
        const sa::EngineConfig c = parse(old);
        check(!c.level_voices && c.filter_heels_ambients && c.filter_hoof_ambient, "soften_loud_barks=0 of an old file turns off level_voices");
        check(parse("[Tweaks]\nsoften_loud_barks=0\nlevel_voices=1\n").level_voices && parse("[Tweaks]\nsoften_loud_barks=1\n").level_voices,
              "... unless the new key says otherwise");
        const std::string now = sa::format_ini(c, old);
        check(now.find("soften_loud_barks") == std::string::npos && now.find("filter_odd_ambients") == std::string::npos &&
              now.find("level_voices=0") != std::string::npos && now.find("mine=1") != std::string::npos, "... and the old keys are gone once it is saved");
    }
    sa::EngineConfig evil = odd;
    evil.sofa_file = "a\r\n[General]\r\nenabled=1";
    check(!parse(sa::format_ini(evil)).enabled, "A file name cannot add lines");

    // On disk.
    TempDir dir;
    const std::wstring path = dir.path + L"\\settings.ini";
    std::string err, read;
    check(sa::save_settings(path, odd, err) && sa::read_file(path, read, 1 << 20) && same(parse(read), odd), "sa_save writes a new file");
    check(sa::save_settings(path, def, err) && sa::read_file(path, read, 1 << 20) && same(parse(read), def), "... and replaces it");
    check(GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES, "... leaving no temporary file");
    {
        // Somebody holds the file: the old one stays as it was.
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(!sa::save_settings(path, odd, err) && !err.empty(), "A locked file is reported");
        CloseHandle(h);
        check(sa::read_file(path, read, 1 << 20) && same(parse(read), def) &&
              GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES, "... and is still whole, without leftovers");
    }
    err.clear();
    check(!sa::save_settings(dir.path + L"\\no\\such\\folder.ini", odd, err) && !err.empty(), "A folder that does not exist is reported");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    err.clear();
    check(!sa::save_settings(path, odd, err) && !err.empty() && sa::read_file(path, read, 1 << 20) && same(parse(read), def),
          "A read-only file is respected");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    // The file that ships.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring ini = sa::folder_above(exe, 1) + L"\\auspex_audio.ini";
    check(sa::read_file(ini, read, 1 << 20) && read == sa::format_ini(def), "auspex_audio.ini is what sa_save writes for the defaults");
}

void test_buried_sources() {
    std::printf("=== Test 30: Sounds inside a wall ===\n");
    sa::AcousticMeshData mesh = room_mesh();
    add_box(mesh, 190, 210, -400, 400, -300, 300); // a wall across the room
    sa::SkyProbe probe;
    probe.build(mesh);
    const auto src = [](float x, float y, float z) {
        const float p[3] = { x, y, z };
        return sa::source_pos_to_ipl(p);
    };
    const auto moved = [&](IPLVector3 from, IPLVector3 listener) {
        const IPLVector3 to = probe.leave_solid(from, listener, 0.4f);
        const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    check(moved(src(100, 0, 0), src(0, 0, 0)) == 0.0f, "In the open: stays");
    check(moved(src(300, 0, 0), src(0, 0, 0)) == 0.0f, "Behind the wall: stays behind it");
    check(moved(src(188, 0, 0), src(0, 0, 0)) == 0.0f, "Right in front of the wall: stays");
    const IPLVector3 out = probe.leave_solid(src(200, 0, 0), src(0, 0, 0), 0.4f);
    const float left = 190.0f * 0.0254f - 0.4f;
    std::printf("    from the middle of a 20 inch wall: %.2f m\n", moved(src(200, 0, 0), src(0, 0, 0)));
    check(nearf(-out.z, left, 0.02f), "Inside the wall: comes out on the listener's side");
    const IPLVector3 other = probe.leave_solid(src(200, 0, 0), src(380, 0, 0), 0.4f);
    check(nearf(-other.z, 210.0f * 0.0254f + 0.4f, 0.02f), "... on either side");
    sa::SkyProbe none;
    sa::AcousticMeshData empty;
    none.build(empty);
    const IPLVector3 p = src(200, 0, 0), q = none.leave_solid(p, src(0, 0, 0), 0.4f);
    check(p.x == q.x && p.y == q.y && p.z == q.z, "No map: stays");

    // Heard: as a sound right in front of that wall, not as one behind it.
    std::string summary;
    Camera cam;
    auto eng = make_engine(false);
    eng->load_scene(mesh, summary);
    publish(*eng, cam);
    Noise noise;
    feed(*eng, 0, source_at(200, 0, 0), noise, 4410);
    run_ticks(*eng, 2);
    IPLDirectEffectParams in_wall{}, behind{};
    check(eng->get_channel_direct_params(0, in_wall) && in_wall.occlusion > 0.9f, "Music in the wall of a club is not buried");
    sa::ChannelDescriptor far_side = source_at(300, 0, 0);
    far_side.mixer = 2;
    feed(*eng, 1, far_side, noise, 4410);
    run_ticks(*eng, 2);
    check(eng->get_channel_direct_params(1, behind) && behind.occlusion < 0.1f, "What is behind the wall still is");
}

// Steam Audio's reverb does not clear the memory it gets (see phonon_memory.h): what a game's heap looks like after a while.
void dirty_heap() {
    static unsigned seed = 7;
    const auto next = [&] { return (seed = seed * 1664525u + 1013904223u) >> 8; };
    std::vector<void*> blocks;
    for (int k = 0; k < 3000; ++k) {
        const SIZE_T n = 16 + (next() % 64) * ((k % 5) ? 64 : 1024);
        void* p = HeapAlloc(GetProcessHeap(), 0, n);
        if (!p) continue;
        for (SIZE_T i = 0; i < n / 4; ++i) static_cast<float*>(p)[i] = 1e10f;
        blocks.push_back(p);
    }
    for (std::size_t i = blocks.size() - 1; i > 0; --i) std::swap(blocks[i], blocks[next() % (i + 1)]);
    for (void* p : blocks) HeapFree(GetProcessHeap(), 0, p);
}

void test_dirty_memory() {
    std::printf("=== Test 31: The reverb starts clean, whatever its memory held before ===\n");
    const sa::AcousticMeshData mesh = room_mesh();
    std::string summary, err;
    Camera cam;
    for (int mode = 0; mode < 2; ++mode) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.reverb_enabled = true;
        cfg.reverb_mode = mode == 1 ? sa::ReverbMode::kHybrid : sa::ReverbMode::kParametric;
        int bad = 0;
        uint32_t faults = 0;
        sa::SteamAudioEngine eng;
        for (int k = 0; k < 40; ++k) {
            dirty_heap();
            if (k % 10 == 0) { // from scratch, and by a rebuild
                eng.shutdown();
                if (!eng.init(cfg, true, err)) ++bad;
                eng.load_scene(mesh, summary);
                publish(eng, cam);
            } else if (!eng.change_output_sync((k & 1) ? 44100 : 48000, 441, err)) {
                ++bad;
            }
            run_ticks(eng, 1);
            Runner r{ eng, source_at(200.0f, 0.0f, 0.0f, 0.005f), Tone{} };
            r.d.mixer = 100 + k;
            float p = peak_of(r.run(8));
            run_ticks(eng, 1);
            p = std::max(p, peak_of(r.run(8)));
            if (!(p > 0.05f && p < 0.35f)) ++bad;
            eng.notify_channel_freed(0);
            faults += eng.get_stats().reverb_faults;
        }
        std::printf("    %s: %d of 40 bad, %u faulty blocks\n", mode == 1 ? "hybrid" : "parametric", bad, faults);
        check(bad == 0 && faults == 0, mode == 1 ? "Hybrid reverb" : "Parametric reverb");
    }
}

void test_device_thread() {
    std::printf("=== Test 32: The device thread on the real output device (skipped without one) ===\n");
    sa::EngineConfig cfg{};
    sa::SteamAudioEngine eng;
    std::string err, msg;
    if (!eng.init(cfg, /*headless=*/false, err)) {
        std::printf("    skipped: %s\n", err.c_str());
        return;
    }
    const sa::AcousticMeshData mesh = room_mesh();
    eng.load_scene(mesh, err);
    Camera cam;
    const auto wait_message = [&](const char* what) {
        for (int i = 0; i < 300; ++i) {
            publish(eng, cam);
            feed_silence(eng, 441);
            if (eng.pop_message(msg)) {
                std::printf("    %s\n", msg.c_str());
                if (msg.find(what) != std::string::npos) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    const auto runs = [&] {
        const uint64_t before = eng.get_stats().blocks_rendered;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return eng.get_stats().device_started && eng.get_stats().blocks_rendered > before + 20;
    };
    check(runs() && !eng.get_stats().device_name.empty(), "The device runs");
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    check(eng.get_stats().device_changes == 0, "A device that is fine is never opened again");
    eng.reopen_output();
    check(wait_message("opened again") && runs() && eng.get_stats().device_changes == 1, "The device is opened again, and runs");
    eng.request_reverb_mode(sa::ReverbMode::kParametric);
    check(wait_message("parametric") && runs() && eng.get_stats().dsp_rebuilds == 1, "Steam Audio is rebuilt under it, and runs");
    eng.request_hrtf("missing.sofa", L"C:\\no such folder\\missing.sofa");
    check(wait_message("missing.sofa") && runs() && eng.get_stats().dsp_rebuilds == 1, "A bad HRTF file interrupts nothing");
    // All at once, with the game going on.
    for (int i = 0; i < 6; ++i) {
        eng.request_reverb_mode((i & 1) ? sa::ReverbMode::kParametric : sa::ReverbMode::kHybrid);
        eng.request_hrtf(std::string(), std::wstring());
        if (i == 3) eng.reopen_output();
        eng.unload_scene();
        eng.load_scene(mesh, err);
        Noise quiet;
        sa::ChannelDescriptor d = source_at(100, 0, 0);
        d.master_gain = 0.0f;
        feed(eng, i, d, quiet, 441);
        eng.set_focused((i & 1) != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    eng.set_focused(true);
    for (int i = 0; i < 100 && eng.get_stats().rebuild_pending; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(!eng.get_stats().rebuild_pending && runs(), "Many changes at once: it settles and runs");
    check(eng.get_stats().reverb_mode == sa::ReverbMode::kParametric, "... with the last settings");
    eng.request_reverb_mode(sa::ReverbMode::kHybrid);
    const auto t0 = std::chrono::steady_clock::now();
    eng.shutdown(); // in the middle of that
    check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3), "It stops promptly, whatever it does");
    check(eng.init(cfg, /*headless=*/false, err) && runs(), "... and can start again");
    eng.shutdown();
}

// ---- The game's own sounds, put right ----

double db_of(double power) { return 10.0 * std::log10(std::max(power, 1e-20)); }

// Power of both ears and of their treble (first difference) over the next `frames`.
struct Spectrum { double power, treble, left, right; };
Spectrum measure(sa::SteamAudioEngine& eng, int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    eng.render_output(out.data(), static_cast<uint32_t>(frames));
    Spectrum s{};
    for (int i = 1; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const double x = out[static_cast<std::size_t>(i) * 2 + c], p = out[static_cast<std::size_t>(i - 1) * 2 + c];
            (c == 0 ? s.left : s.right) += x * x;
            s.treble += (x - p) * (x - p);
        }
    }
    s.power = s.left + s.right;
    return s;
}

// Keeps `seconds` of sound going through a channel and measures the last tenth of a second.
template <class Gen>
Spectrum settle(sa::SteamAudioEngine& eng, int ch, const sa::ChannelDescriptor& d, Gen& gen, double seconds) {
    const int steps = static_cast<int>(seconds * 100.0);
    const int out = eng.output_rate() / 100;
    feed(eng, ch, d, gen, 4410);
    for (int i = 0; i < steps; ++i) {
        feed(eng, ch, d, gen, 441);
        render(eng, out);
    }
    feed(eng, ch, d, gen, 4410);
    return measure(eng, out * 10);
}

struct LoudNoise { // white, at a given RMS level
    Noise n;
    float rms;
    explicit LoudNoise(float db) : rms(std::pow(10.0f, db / 20.0f)) {}
    void next(float* out, int count) {
        n.next(out, count);
        for (int i = 0; i < count; ++i) out[i] *= rms / 0.14434f; // uniform +-0.25
    }
};

sa::ChannelSnapshot bark_snapshot() {
    sa::ChannelSnapshot c{}; // sm_hub_1 "prostitute_2": "Hey, baby. Lookin' for a date?"
    c.name = "character\\female\\hooker2\\float_3.wav";
    c.entnum = 212;
    c.entchannel = sa::kChanVoice;
    c.origin[0] = 100.0f;
    c.origin[1] = -120.0f;
    c.dist_mult = 0.0f; // SNDLVL_NONE
    c.master_vol = 255;
    c.volumes[0] = c.volumes[1] = 127;
    c.distance = 156.0f;
    c.skip_collide = true; // comes with SNDLVL_NONE
    c.mouth_height = 63.0f;
    c.has_facing = true;
    c.facing[0] = -1.0f;
    return c;
}

void test_barks() {
    std::printf("=== Test 33: Passers-by speak from where they stand ===\n");
    const sa::GainModel gm;
    check(sa::is_bark_sound("character\\male\\citizen_2\\float_12.wav") && sa::is_bark_sound("*Character/Female/hooker1/float_1.wav") &&
          !sa::is_bark_sound("character/dlg/santa monica/trip/line1_col_e.mp3") && !sa::is_bark_sound("character/monster/roar.wav") &&
          !sa::is_bark_sound("character/conversations/answering machine/a.wav") && !sa::is_bark_sound("interface/infobar/frenzy.wav") &&
          !sa::is_bark_sound(nullptr), "Barks are recognized by name");
    check(nearf(sa::level_for_dist_mult(sa::dist_mult_for_level(80.0f, gm), gm), 80.0f, 1e-3f) &&
          nearf(sa::dist_mult_for_level(70.0f, gm), dist_mult_for(70, gm.ref_dist, gm.ref_db), 1e-7f), "Levels and dist_mult convert as in engine.dll");

    const sa::ChannelSnapshot bark = bark_snapshot();
    sa::ChannelDescriptor d = sa::describe_channel(bark, gm);
    check(d.spatial_3d && nearf(d.dist_mult, dist_mult_for(70, gm.ref_dist, gm.ref_db), 1e-7f) && nearf(d.master_gain, 1.0f),
          "The prostitutes' line, SNDLVL_NONE in the game, is a 70 dB voice in the world");
    check(nearf(d.origin[2], 63.0f) && d.directional && d.reverb_gain > 0.1f, "... from the mouth, with a direction, in the room");
    check(!d.no_occlusion && !d.dialogue, "... and can be behind a wall: it is no conversation");
    sa::ChannelSnapshot flagged = bark;
    flagged.is_voice = true; // recordings with a sentence in their .lip file
    d = sa::describe_channel(flagged, gm);
    check(d.spatial_3d && !d.dialogue && !d.no_occlusion && d.is_voice, "The same if the recording counts as a voice");
    sa::ChannelSnapshot far_away = bark;
    far_away.mouth_height = 0.0f; // not known to the client
    far_away.has_facing = false;
    check(sa::describe_channel(far_away, gm).spatial_3d, "... or who says it is out of sight");

    sa::ChannelSnapshot x = bark;
    x.own = true;
    check(!sa::describe_channel(x, gm).spatial_3d, "The player's own voice stays in the head");
    x = bark;
    x.player = true;
    check(!sa::describe_channel(x, gm).spatial_3d, "... also while the view is somewhere else");
    x = bark;
    x.name = "character/dlg/hollywood/theatre/line4_col_e.mp3";
    x.entchannel = sa::kChanStream;
    x.is_voice = true;
    check(!sa::describe_channel(x, gm).spatial_3d, "Cutscene lines with \"full_sound\" keep playing everywhere");
    x = bark;
    x.name = "character/conversations/answering machine/mercurio.wav";
    check(!sa::describe_channel(x, gm).spatial_3d, "... and phones, radios and machines");
    x.name = "interface/infobar/frenzy.wav";
    check(!sa::describe_channel(x, gm).spatial_3d, "... and the interface");
    x.name = "character/monster/werewolf/roar.wav";
    check(!sa::describe_channel(x, gm).spatial_3d, "... and bosses that are meant to be heard everywhere");
    x = bark;
    x.origin[0] = std::nanf("");
    check(!sa::describe_channel(x, gm).spatial_3d, "Without a position there is nothing to put right");
    sa::ChannelTweaks off;
    off.npc_barks = false;
    d = sa::describe_channel(bark, gm, off);
    check(!d.spatial_3d && d.no_occlusion && nearf(d.master_gain, 1.0f), "fix_npc_barks=0 is the original game");

    // Through the engine: she stands to the right.
    const auto ears = [&](const sa::ChannelTweaks& t) {
        auto eng = make_engine();
        Camera cam;
        cam.org[2] = 64.0f;
        publish(*eng, cam);
        sa::ChannelDescriptor say = sa::describe_channel(bark, gm, t);
        say.mixer = 4;
        LoudNoise voice(-24.0f);
        return settle(*eng, 2, say, voice, 0.3);
    };
    const Spectrum now = ears(sa::ChannelTweaks()), before = ears(off);
    std::printf("    right ear over left: %.1f dB (original %.1f dB)\n", db_of(now.right / now.left), db_of(before.right / before.left));
    check(std::fabs(db_of(before.right / before.left)) < 0.1, "In both ears before");
    check(db_of(now.right / now.left) > 4.0, "Now it comes from where she stands");

    // Stage lines of street characters, at 80 dB in the game.
    sa::ChannelSnapshot prophet{};
    prophet.name = "*character\\dlg\\generic\\prophet\\line3_col_e.mp3";
    prophet.entnum = 90;
    prophet.entchannel = sa::kChanStream;
    prophet.dist_mult = dist_mult_for(80, gm.ref_dist, gm.ref_db);
    prophet.master_vol = 255;
    prophet.is_voice = true;
    prophet.distance = 400.0f;
    d = sa::describe_channel(prophet, gm);
    const auto gain_db = [&](float dist_mult, float meters) {
        return 20.0f * std::log10(sa::compute_source_distance_attenuation(meters, dist_mult, gm));
    };
    std::printf("    prophet: %.1f dB, change at 1.5 m %.1f dB, at 4 m %.1f, at 10 m %.1f, at 20 m %.1f\n", sa::level_for_dist_mult(d.dist_mult, gm),
                gain_db(d.dist_mult, 1.5f) - gain_db(prophet.dist_mult, 1.5f), gain_db(d.dist_mult, 4.0f) - gain_db(prophet.dist_mult, 4.0f),
                gain_db(d.dist_mult, 10.0f) - gain_db(prophet.dist_mult, 10.0f), gain_db(d.dist_mult, 20.0f) - gain_db(prophet.dist_mult, 20.0f));
    check(nearf(sa::level_for_dist_mult(d.dist_mult, gm), 75.0f, 0.01f) && d.leveling == sa::Leveling::kBark, "The prophet by the diner falls off like a shout");
    check(gain_db(d.dist_mult, 1.5f) - gain_db(prophet.dist_mult, 1.5f) > -0.5f, "... but is as loud as ever next to him");
    prophet.skip_collide = true;
    d = sa::describe_channel(prophet, gm);
    check(d.dialogue && d.leveling == sa::Leveling::kDialogue && nearf(d.dist_mult, prophet.dist_mult, 1e-9f),
          "Talking to him is leveled like any conversation, and not moved");
    prophet.skip_collide = false;
    prophet.name = "*character/dlg/main characters/lacroix/line3_col_e.mp3";
    d = sa::describe_channel(prophet, gm);
    check(d.leveling == sa::Leveling::kDialogue && nearf(d.dist_mult, prophet.dist_mult, 1e-9f), "So are the scenes of the story");
    sa::ChannelSnapshot passer = bark;
    passer.name = "character/male/citizen_2/float_3.wav";
    passer.dist_mult = dist_mult_for(70, gm.ref_dist, gm.ref_db);
    passer.skip_collide = false;
    d = sa::describe_channel(passer, gm);
    check(nearf(d.dist_mult, passer.dist_mult, 1e-9f), "Normal passers-by are as loud as ever at any distance");
    off = sa::ChannelTweaks();
    off.level_voices = false;
    prophet.name = "character/dlg/generic/prophet/line3_col_e.mp3";
    d = sa::describe_channel(prophet, gm, off);
    check(d.leveling == sa::Leveling::kNone && nearf(d.dist_mult, prophet.dist_mult, 1e-9f), "level_voices=0 is the original game");
}

void test_dynamics() {
    std::printf("=== Test 34: Levelers and the limiter ===\n");
    check(sa::compressor_gain_db(-30, -16, 4, 4, 10) == 0.0f && nearf(sa::compressor_gain_db(-8, -16, 4, 0, 20), -6.0f, 1e-4f) &&
          nearf(sa::compressor_gain_db(0, -16, 4, 0, 10), -10.0f, 1e-4f) && sa::compressor_gain_db(0, -16, 1, 0, 10) == 0.0f,
          "The compressor's curve: threshold, ratio, most it takes off");
    float prev = 0.0f;
    bool falls = true, even = true;
    for (float l = -40.0f; l <= 6.0f; l += 0.1f) {
        const float g = sa::compressor_gain_db(l, -16.5f, 5.0f, 4.0f, 8.0f);
        if (g > prev + 1e-5f) falls = false;
        if (prev - g > 0.09f) even = false;
        if (l + g < -40.0f) falls = false;
        prev = g;
    }
    check(falls && even, "... without a step");

    const float rate = 48000.0f;
    const auto level = [&](float db, const sa::LevelerSettings& s, float scale, bool& same, float freq) {
        sa::LoudnessLeveler lev;
        double e0 = 0.0, e1 = 0.0;
        double phase = 0.0;
        same = true;
        const float amp = std::pow(10.0f, db / 20.0f) * 1.41421356f;
        for (int b = 0; b < 400; ++b) {
            float x[128], y[128];
            for (int i = 0; i < 128; ++i) {
                x[i] = y[i] = amp * static_cast<float>(std::sin(phase));
                phase += 6.283185307 * freq / rate;
            }
            lev.process(y, nullptr, 128, rate, s, scale);
            for (int i = 0; i < 128; ++i) {
                if (x[i] != y[i]) same = false;
                if (b >= 200) {
                    e0 += static_cast<double>(x[i]) * x[i];
                    e1 += static_cast<double>(y[i]) * y[i];
                }
            }
        }
        return 10.0 * std::log10(e1 / e0);
    };
    bool same = false, ignored = false;
    const double frenzy_mid = level(-17.3f, sa::kStingerLeveler, 1.0f, ignored, 1000.0f);
    const double frenzy_low = level(-17.3f, sa::kStingerLeveler, 1.0f, ignored, 60.0f);
    level(-17.3f, sa::kStingerLeveler, 0.2f, same, 1000.0f);
    std::printf("    2D effects at frenzy's level: %.1f dB, its bass alone %.1f dB\n", frenzy_mid, frenzy_low);
    check(frenzy_mid < -2.5 && frenzy_mid > -5.0 && frenzy_low < frenzy_mid - 2.5 && frenzy_low > -12.1, "Frenzy: calmer, its bass more so");
    check(same, "... but not if the game plays it quietly");
    level(-20.0f, sa::kBassGuard, 1.0f, same, 60.0f);
    check(same, "The bass guard leaves normal bass alone, bit for bit");
    level(-10.0f, sa::kBassGuard, 1.0f, same, 2000.0f);
    check(same, "... and everything that is no bass");
    const double boom = level(-8.0f, sa::kBassGuard, 1.0f, ignored, 60.0f);
    std::printf("    bass guard: 60 Hz at -8 dBFS %.1f dB\n", boom);
    check(boom < -3.0 && boom > -9.1, "Stacked bass is held back");
    {
        sa::LoudnessLeveler lev;
        float bad[128];
        for (int i = 0; i < 128; ++i) bad[i] = std::nanf("");
        lev.process(bad, nullptr, 128, rate, sa::kStingerLeveler, 1.0f);
        float fine[128], copy[128];
        bool ok = true;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < 128; ++i) fine[i] = copy[i] = 0.001f * std::sin(0.1f * static_cast<float>(i));
            lev.process(fine, nullptr, 128, rate, sa::kStingerLeveler, 1.0f);
            for (int i = 0; i < 128; ++i) ok = ok && std::isfinite(fine[i]);
        }
        check(ok && std::memcmp(fine, copy, sizeof(fine)) == 0, "A broken block is forgotten");
        float l[128], r[128];
        sa::LoudnessLeveler stereo;
        float ratio_l = 0.0f, ratio_r = 0.0f;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < 128; ++i) {
                l[i] = 0.8f * std::sin(0.13f * static_cast<float>(b * 128 + i));
                r[i] = 0.1f * l[i];
            }
            const float before = l[31];
            stereo.process(l, r, 128, rate, sa::kStingerLeveler, 1.0f);
            ratio_l = l[31] / before;
            ratio_r = r[31] / (0.1f * before);
        }
        check(nearf(ratio_l, ratio_r, 1e-4f) && ratio_l < 0.9f, "Both sides get the same gain: the image does not move");
    }

    const auto limit = [&](float amp, int blocks, sa::PeakLimiter& lim, std::vector<float>& out) {
        out.clear();
        for (int b = 0; b < blocks; ++b) {
            float x[256];
            for (int i = 0; i < 128; ++i) {
                x[i * 2] = amp * std::sin(6.2831853f * 375.0f * static_cast<float>(b * 128 + i) / rate); // whole periods
                x[i * 2 + 1] = 0.5f * x[i * 2];
            }
            lim.process(x, 128, rate);
            out.insert(out.end(), x, x + 256);
        }
    };
    std::vector<float> out;
    {
        sa::PeakLimiter lim;
        limit(0.84f, 20, lim, out);
        bool exact = lim.gain() == 1.0f;
        for (std::size_t i = 0; i < out.size() / 2 && exact; ++i) {
            exact = out[i * 2] == 0.84f * std::sin(6.2831853f * 375.0f * static_cast<float>(i) / rate);
        }
        check(exact, "What fits passes the limiter bit for bit");
    }
    {
        sa::PeakLimiter lim;
        limit(1.6f, 100, lim, out);
        float peak = 0.0f;
        double residual = 0.0, power = 0.0;
        const std::size_t n = out.size() / 2;
        for (std::size_t i = n / 2; i < n; ++i) {
            peak = std::max(peak, std::fabs(out[i * 2]));
            const double ideal = 0.85 * std::sin(6.2831853 * 375.0 * static_cast<double>(i) / rate);
            residual += (out[i * 2] - ideal) * (out[i * 2] - ideal);
            power += ideal * ideal;
        }
        std::printf("    5.5 dB over: peak %.3f, distortion %.1f dB\n", peak, db_of(residual / power));
        check(peak <= 0.851f && db_of(residual / power) < -50.0, "What does not is turned down, not bent");
        limit(0.1f, 400, lim, out);
        check(lim.gain() == 1.0f, "... and comes back up");
        float nan[256];
        for (float& f : nan) f = std::nanf("");
        lim.process(nan, 128, rate);
        check(lim.gain() == 1.0f, "Broken samples do not move it");
        float spike[256] = {};
        spike[200] = 30.0f;
        lim.process(spike, 128, rate);
        check(lim.gain() >= sa::PeakLimiter::kMinGain, "One spike cannot mute the rest");
    }
}

void test_loud_sounds() {
    std::printf("=== Test 35: Frenzy over combat music ===\n");
    sa::ChannelSnapshot frenzy{}; // CHudInfoBar: view entity, CHAN_AUTO, attenuation 0, SND 0x2800
    frenzy.name = "interface/infobar/frenzy.wav";
    frenzy.entnum = 1;
    frenzy.master_vol = 255;
    frenzy.volumes[0] = frenzy.volumes[1] = 127;
    frenzy.own = frenzy.player = frenzy.dry = true;
    sa::ChannelDescriptor d = sa::describe_channel(frenzy);
    check(!d.spatial_3d && d.leveling == sa::Leveling::kStinger && d.reverb_gain == 0.0f && nearf(d.master_gain, 1.0f),
          "Frenzy is an effect in the head: no reverb, once, evened out");
    sa::ChannelTweaks off;
    off.tame_loud = false;
    check(sa::describe_channel(frenzy, sa::GainModel(), off).leveling == sa::Leveling::kNone, "tame_loud_sounds=0 leaves it alone");
    sa::ChannelSnapshot other = frenzy;
    other.name = "music/default_combat.mp3";
    check(sa::describe_channel(other).leveling == sa::Leveling::kNone, "Music is not");
    other.name = "weapons/pistol/fire.wav";
    other.volumes[0] = other.volumes[1] = 255;
    d = sa::describe_channel(other);
    check(d.leveling == sa::Leveling::kNone && nearf(d.master_gain, sa::kMaxGain2D), "Nor the player's guns, which get at most the engine's +2 dB");
    other = frenzy;
    other.name = "disciplines/protean/activate.wav";
    other.own = other.player = false;
    other.dist_mult = 0.001f;
    check(sa::describe_channel(other).leveling == sa::Leveling::kNone, "Nor what others do in the world");

    // A low drone with hiss at frenzy's level, and music with a heavy bass, as the game plays both at once.
    struct Drone {
        Noise n;
        double p0 = 0.0, p1 = 0.0;
        float low, high;
        Drone(float l, float h) : low(l), high(h) {}
        void next(float* out, int count) {
            n.next(out, count);
            for (int i = 0; i < count; ++i) {
                out[i] = out[i] * high + low * static_cast<float>(std::sin(p0) + 0.7 * std::sin(p1));
                p0 += 6.283185307 * 55.0 / 44100.0;
                p1 += 6.283185307 * 92.0 / 44100.0;
            }
        }
    };
    struct Both { double power, bass; float peak; bool finite; };
    const auto play = [&](bool tame, bool with_music, float music_volume = 0.7f) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.tame_loud_sounds = tame;
        auto eng = make_engine(false, cfg);
        sa::ChannelTweaks t;
        t.tame_loud = tame;
        sa::ChannelDescriptor f = sa::describe_channel(frenzy, sa::GainModel(), t);
        f.mixer = 1;
        sa::ChannelDescriptor m{};
        m.active = true;
        m.master_gain = music_volume;
        m.mixer = 2;
        Drone drone(0.085f, 0.80f), music(0.45f, 0.5f); // -17 and -10 dBFS, a quarter and two thirds of it bass
        Both r{ 0.0, 0.0, 0.0f, true };
        float z = 0.0f;
        std::vector<float> out(480 * 2);
        for (int i = 0; i < 100; ++i) {
            float a[1020], b[1020];
            drone.next(a, 441);
            music.next(b, 441);
            eng->begin_chunk(1020);
            eng->feed_channel_mono(0, f, a, 0, 441);
            if (with_music) eng->feed_channel_mono(1, m, b, 0, 441);
            eng->end_chunk(441);
            if (i < 8) continue;
            eng->render_output(out.data(), 480);
            for (int s = 0; s < 480 && i >= 30; ++s) {
                const float x = out[static_cast<std::size_t>(s) * 2];
                if (!std::isfinite(x)) r.finite = false;
                r.peak = std::max(r.peak, std::fabs(x));
                r.power += static_cast<double>(x) * x;
                z += (x - z) * (1.0f - std::exp(-6.2831853f * 150.0f / 48000.0f));
                r.bass += static_cast<double>(z) * z;
            }
        }
        return r;
    };
    const Both before = play(false, true), after = play(true, true), alone_before = play(false, false), alone = play(true, false);
    std::printf("    together: bass %.1f dB, all %.1f dB, peak %.2f -> %.2f | frenzy alone: bass %.1f dB, all %.1f dB\n",
                db_of(after.bass / before.bass), db_of(after.power / before.power), before.peak, after.peak,
                db_of(alone.bass / alone_before.bass), db_of(alone.power / alone_before.power));
    check(after.finite && after.peak <= 0.86f, "Nothing clips");
    check(db_of(after.bass / before.bass) < -4.0, "The boom is gone");
    check(db_of(alone.bass / alone_before.bass) < -4.0 && db_of(alone.bass / alone_before.bass) > -10.0 &&
          db_of(alone.power / alone_before.power) < -2.0 && db_of(alone.power / alone_before.power) > -6.0, "Frenzy alone is calmer, and still there");
    const Both hot = play(true, true, 1.0f);
    check(hot.finite && hot.peak <= 0.86f, "... also with the music at full volume");

    // A voice that is as loud as all voices is not touched by any of this.
    const auto say = [&](bool tweaks) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.tame_loud_sounds = cfg.level_voices = tweaks;
        auto eng = make_engine(false, cfg);
        Camera cam;
        publish(*eng, cam);
        sa::ChannelDescriptor v = source_at(60, 0, 0, dist_mult_for(70, 50));
        v.is_voice = true;
        if (tweaks) v.leveling = sa::Leveling::kBark;
        LoudNoise voice(sa::kVoiceTargetDb - 3.3f); // white noise measures 3.3 dB more than its RMS
        feed(*eng, 0, v, voice, 12000);
        std::vector<float> out(8000);
        eng->render_output(out.data(), 4000);
        return out;
    };
    const std::vector<float> leveled = say(true), plain = say(false);
    double was = 0.0, is = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        was += static_cast<double>(plain[i]) * plain[i];
        is += static_cast<double>(leveled[i]) * leveled[i];
    }
    std::printf("    a normal voice: %+.2f dB\n", db_of(is / was));
    check(std::fabs(db_of(is / was)) < 0.5, "A passer-by with a normal voice sounds the same");
}

void test_rain_masking() {
    std::printf("=== Test 36: Rain covers what is far away ===\n");
    sa::ChannelDescriptor rain = sa::describe_channel(rain_snapshot());
    rain.mixer = 1;
    check(rain.rain > 0.99f, "The hub's rain is full rain");
    sa::ChannelSnapshot w = rain_snapshot();
    w.name = "area/santa_monica/thunder_heavy1.wav";
    check(sa::describe_channel(w).weather > 0.99f && sa::describe_channel(w).rain == 0.0f, "Thunder is weather, but covers nothing");
    w = rain_snapshot();
    w.master_vol = 51;
    w.volumes[0] = w.volumes[1] = 25;
    check(sa::describe_channel(w).rain > 0.3f && sa::describe_channel(w).rain < 0.6f, "Half as loud, half the rain");

    check(sa::is_outdoor_ambient("environmental/city/Freeway Overpass.wav") && sa::is_outdoor_ambient("Environmental\\Vehicles\\truck_passing_3.wav") &&
          sa::is_outdoor_ambient("Area\\Special\\Tutorial\\Pedestrianby4.wav") && sa::is_outdoor_ambient("area/Santa_Monica/santa monica main bg.mp3") &&
          sa::is_outdoor_ambient("environmental/city/horn 2.wav"), "Traffic, horns, steps and the city's background are outdoors");
    check(!sa::is_outdoor_ambient("environmental/interior/room_small.wav") && !sa::is_outdoor_ambient("music/0hdoverhaul/hub night streets.mp3") &&
          !sa::is_outdoor_ambient("area/Santa_Monica/rain_light_loop.wav") && !sa::is_outdoor_ambient("environmental/city/elegantrestaurant.wav") &&
          !sa::is_outdoor_ambient("interface/click.wav") && !sa::is_outdoor_ambient("character/male/float_1.wav") &&
          !sa::is_outdoor_ambient("environmental/city/police_radio1.wav") && !sa::is_outdoor_ambient(nullptr),
          "... rooms, music, the rain itself, interface, voices and radios are not");

    const sa::AcousticMeshData mesh = street_mesh();
    const auto play = [&](const sa::ChannelDescriptor& what, float where, bool raining, float strength) {
        sa::EngineConfig cfg{};
        cfg.sample_rate = 48000;
        cfg.rain_masking = strength;
        auto eng = make_engine(false, cfg);
        std::string summary;
        eng->load_scene(mesh, summary);
        Camera cam;
        cam.org[0] = where;
        cam.org[2] = 64.0f;
        publish(*eng, cam);
        eng->run_simulation_sync();
        // The rain plays silently here: only what it does to the others is measured.
        sa::ChannelDescriptor r = rain;
        LoudNoise voice(-26.0f);
        const std::vector<float> silence(441, 0.0f);
        std::vector<float> buf(441);
        Spectrum last{};
        for (int i = 0; i < 620; ++i) {
            voice.next(buf.data(), 441);
            eng->begin_chunk(1020);
            if (raining) eng->feed_channel_mono(0, r, silence.data(), 0, 441);
            eng->feed_channel_mono(1, what, buf.data(), 0, 441);
            eng->end_chunk(441);
            if (i >= 10) last = measure(*eng, 480);
        }
        return last;
    };
    const auto voice_at = [&](float from, float meters) {
        sa::ChannelDescriptor v = source_at(from - meters / sa::kSourceToMeters, 0, 64, 1e-7f); // the same level anywhere
        v.is_voice = true;
        v.no_occlusion = true;
        return v;
    };
    struct Change { double power, treble; };
    const auto change = [&](const sa::ChannelDescriptor& what, float where, float strength = 1.0f) {
        const Spectrum dry = play(what, where, false, strength), wet = play(what, where, true, strength);
        return Change{ db_of(wet.power / dry.power), db_of(wet.treble / dry.treble) };
    };
    const Change near_voice = change(voice_at(-400, 1.5f), -400), mid_voice = change(voice_at(-400, 6.0f), -400), far_voice = change(voice_at(-400, 14.0f), -400);
    std::printf("    voice in the rain at 1.5 m: %.1f dB (treble %.1f), 6 m: %.1f (%.1f), 14 m: %.1f (%.1f)\n", near_voice.power,
                near_voice.treble, mid_voice.power, mid_voice.treble, far_voice.power, far_voice.treble);
    check(std::fabs(near_voice.power) < 0.05 && std::fabs(near_voice.treble) < 0.05, "Who stands next to the player is as clear as ever");
    check(mid_voice.power < -0.5 && mid_voice.power > -4.0 && far_voice.power < mid_voice.power - 1.0 && far_voice.power > -14.0, "Further off it gets softer");
    check(far_voice.treble < far_voice.power - 1.0, "... and duller");
    sa::ChannelDescriptor talk = voice_at(-400, 14.0f);
    talk.dialogue = true;
    const Change said = change(talk, -400);
    check(said.power == 0.0 && said.treble == 0.0, "A conversation stays bit for bit the same");
    const Change inside = change(voice_at(700, 6.0f), 700);
    std::printf("    deep inside a building: %.2f dB\n", inside.power);
    check(std::fabs(inside.power) < 0.3, "Inside the rain covers nothing");
    const Change off = change(voice_at(-400, 14.0f), -400, 0.0f), less = change(voice_at(-400, 14.0f), -400, 0.5f);
    check(off.power == 0.0 && less.power < -1.0 && less.power > far_voice.power + 1.0, "rain_masking sets how much");

    sa::ChannelDescriptor city{};
    city.active = true;
    city.outdoor_ambient = true;
    city.master_gain = 0.4f;
    city.mixer = 7;
    const Change bg = change(city, -400);
    sa::ChannelDescriptor traffic = voice_at(-400, 3.0f);
    traffic.is_voice = false;
    traffic.outdoor_ambient = true;
    const Change cars = change(traffic, -400), bg_inside = change(city, 700);
    sa::ChannelDescriptor music = city;
    music.outdoor_ambient = false;
    const Change tune = change(music, -400);
    std::printf("    city background %.1f dB (treble %.1f), traffic 3 m away %.1f (%.1f), inside %.2f, music %.1f\n", bg.power, bg.treble,
                cars.power, cars.treble, bg_inside.power, tune.power);
    check(bg.power < -4.0 && bg.treble < bg.power - 4.0, "The city's background steps back behind the rain");
    check(cars.power < -4.0 && cars.treble < cars.power - 2.0, "... and so does traffic with a place");
    check(std::fabs(bg_inside.power) < 0.4, "... not inside");
    check(tune.power == 0.0, "Music, interface and indoor ambients stay as they are");

    // It comes and goes slowly, and stays off in a drizzle that is hardly there.
    auto eng = make_engine();
    const std::vector<float> silence(441, 0.0f);
    const auto rain_for = [&](int ticks, float amount, float& step) {
        sa::ChannelDescriptor r = rain;
        r.rain = amount;
        float prev = eng->rain();
        for (int i = 0; i < ticks; ++i) {
            eng->begin_chunk(1020);
            if (amount > 0.0f) eng->feed_channel_mono(0, r, silence.data(), 0, 441);
            eng->end_chunk(441);
            render(*eng, 441);
            step = std::max(step, std::fabs(eng->rain() - prev));
            prev = eng->rain();
        }
        return eng->rain();
    };
    float step = 0.0f;
    const float half = rain_for(80, 1.0f, step), full = rain_for(500, 1.0f, step);
    const float gap = rain_for(30, 0.0f, step); // the loop starts over
    const float over = rain_for(1500, 0.0f, step);
    std::printf("    after 0.8 s %.2f, 6 s %.2f, a 0.3 s gap %.2f, 15 s later %.3f; largest step %.3f\n", half, full, gap, over, step);
    check(half > 0.3f && half < 0.7f && full > 0.98f && gap > 0.85f && over < 0.01f && step < 0.02f, "Rain sets in and ends gradually, a gap in its loop is not heard");
    const float drizzle = rain_for(500, 0.06f, step);
    check(drizzle == 0.0f || drizzle < 0.005f, "A trace of rain does not count");
    rain_for(500, 0.3f, step);
    const float stays = rain_for(800, 0.06f, step);
    check(stays > 0.05f, "... unless it was raining: no flutter on the edge");
}

void test_dialogue_glass() {
    std::printf("=== Test 37: Conversations through protective glass ===\n");
    check(sa::surface_is_pane("maps/sm_pawnshop_2/GLASS/GLASSC") && sa::surface_is_pane("glass/glasssmokeshop") &&
          !sa::surface_is_pane("lights/fluorescent") && !sa::surface_is_pane("metal/chainlinka") && !sa::surface_is_pane("wood/trntrim"),
          "Panes are of glass; lamps, nets and frames are none");

    // The pawn shop: Trip behind the counter, a pane from waist to ceiling in front of him, a table with a glass top.
    sa::AcousticMeshData mesh = room_mesh();
    const auto pane = [](std::vector<IPLVector3>& v, std::vector<IPLTriangle>& t, float x0, float x1, float y0, float y1, float z0, float z1) {
        const sa::AcousticBrushPlane box[6] = {
            {  1, 0, 0, x1, sa::kMatGlass }, { -1, 0, 0, -x0, sa::kMatGlass },
            {  0, 1, 0, y1, sa::kMatGlass }, { 0, -1, 0, -y0, sa::kMatGlass },
            {  0, 0, 1, z1, sa::kMatGlass }, { 0, 0, -1, -z0, sa::kMatGlass },
        };
        sa::clip_glass_brush(box, 6, v, t);
    };
    sa::AcousticMeshData plain = mesh; // without the pane
    pane(mesh.glass_vertices, mesh.glass_triangles, 30, 31, -48, 48, 45, 150);
    pane(mesh.glass_vertices, mesh.glass_triangles, 0, 30, -48, 48, 33, 35); // the counter's top
    add_box(mesh, 20, 40, -200, -60, 0, 150);                                  // and a pillar beside it
    add_box(plain, 20, 40, -200, -60, 0, 150);

    sa::GlassProbe probe;
    probe.build(mesh);
    const auto at = [](float x, float y, float z) { return sa::source_pos_to_ipl(x, y, z); };
    const IPLVector3 mouth = at(66, 0, 64);
    check(probe.between(at(-40, 0, 64), mouth, nullptr, 0) == 1.0f, "Through the pane");
    check(probe.between(at(25, 0, 64), mouth, nullptr, 0) == 1.0f && probe.between(at(28, 0, 64), mouth, nullptr, 0) == 1.0f,
          "... with the dialogue camera right in front of it");
    check(probe.between(at(-40, -30, 64), mouth, nullptr, 0) == 1.0f, "... at an angle");
    check(probe.between(at(40, 0, 64), mouth, nullptr, 0) == 0.0f, "Behind the counter: no glass in between");
    check(probe.between(at(-40, 0, 64), at(20, 0, 64), nullptr, 0) == 0.0f, "Who stands in front of it is not muffled");
    check(probe.between(at(-40, -130, 64), at(80, -130, 64), nullptr, 0) == 0.0f, "A pillar is no glass");
    check(probe.between(at(-40, 200, 64), at(80, 200, 64), nullptr, 0) == 0.0f, "Past the pane");
    check(probe.between(at(-40, 0, 64), at(20, 0, 60), nullptr, 0) == 0.0f, "The glass top of a counter below the line does not count");
    const float edge = probe.between(at(-40, 46, 64), at(66, 50, 64), nullptr, 0);
    check(edge >= 0.0f && edge <= 1.0f, "Its edge is no switch");
    sa::GlassProbe none;
    none.build(plain);
    check(none.empty() && none.between(at(-40, 0, 64), mouth, nullptr, 0) == 0.0f, "Maps without glass");

    sa::ChannelSnapshot c{};
    c.name = "*character/dlg/santa monica/trip/line1_col_e.mp3";
    c.entnum = 300;
    c.entchannel = sa::kChanStream;
    c.origin[0] = 66.0f;
    c.dist_mult = dist_mult_for(80, 50);
    c.master_vol = 255;
    c.is_voice = true;
    c.skip_collide = true;
    c.distance = 40.0f;
    c.mouth_height = 64.0f;
    sa::ChannelDescriptor line = sa::describe_channel(c);
    line.mixer = 3;
    struct Heard { Spectrum all; RoomEnergy room; float first; std::vector<float> start; };
    const auto play = [&](const sa::AcousticMeshData& m, bool on, float cam_x, const float* head) {
        sa::EngineConfig cfg{};
        cfg.dialogue_glass = on;
        auto eng = make_engine(true, cfg);
        std::string summary;
        eng->load_scene(m, summary);
        Camera cam;
        cam.org[0] = cam_x;
        cam.org[2] = 64.0f;
        publish(*eng, cam);
        eng->update_player_head(head);
        run_ticks(*eng, 2);
        Heard h{};
        Noise n;
        feed(*eng, 0, line, n, 8820);
        eng->run_simulation_sync();
        const Spectrum first = measure(*eng, 1024);
        h.all = measure(*eng, 4096);
        h.first = static_cast<float>(db_of(first.power / 1024.0) - db_of(h.all.power / 4096.0));
        eng->notify_channel_freed(0);
        render(*eng, 44100);
        h.room = play_in_room(*eng, line);
        return h;
    };
    const Heard glass = play(mesh, true, 26, nullptr), open = play(plain, true, 26, nullptr), off = play(mesh, false, 26, nullptr);
    const double cut = db_of(glass.all.power / open.all.power), dull = db_of(glass.all.treble / open.all.treble);
    std::printf("    through glass: %.1f dB, treble %.1f dB, room %.1f dB below the voice (open %.1f), first 23 ms %.2f dB\n", cut, dull,
                db_of(glass.room.direct / glass.room.tail), db_of(open.room.direct / open.room.tail), glass.first);
    check(cut < -3.0 && cut > -10.0 && dull < cut - 2.0, "The voice is softer and loses its treble");
    check(glass.room.tail < glass.room.direct * 2e-4, "Still no echo");
    check(std::fabs(glass.first) < 1.0f, "From its first moment on: it does not slide behind the glass");
    const auto alike = [](const Spectrum& a, const Spectrum& b) { // the room's rays are random
        return std::fabs(db_of(a.power / b.power)) < 0.1 && std::fabs(db_of(a.treble / b.treble)) < 0.1;
    };
    check(alike(off.all, open.all), "dialogue_glass=0: as without the pane");
    const float front[3] = { -40, 0, 64 }, behind[3] = { 80, 30, 64 };
    const Heard cut_in = play(mesh, true, 45, front), same_side = play(mesh, true, 45, behind);
    check(std::fabs(db_of(cut_in.all.power / play(plain, true, 45, front).all.power) - cut) < 2.5, "A camera cut to his side of the glass changes nothing");
    check(alike(same_side.all, play(plain, true, 45, behind).all), "... unless the player is there, too");

    // The glass of a door, and a conversation that starts before its rays are back.
    sa::AcousticMeshData doors = plain;
    sa::AcousticSubModel door = box_model(1, -1, 1, -40, 40, 0, 45);
    pane(door.glass_vertices, door.glass_triangles, -1, 1, -40, 40, 45, 100);
    doors.submodels.push_back(door);
    auto eng = make_engine();
    std::string summary;
    eng->load_scene(doors, summary);
    Camera cam;
    cam.org[2] = 64.0f;
    publish(*eng, cam);
    sa::BrushEntityState b{};
    b.entity = 20;
    b.model_index = 1;
    b.solid = true;
    b.origin[0] = 30.0f;
    eng->update_brush_entities(&b, 1);
    Noise n;
    feed(*eng, 0, line, n, 8820);
    run_ticks(*eng, 2);
    render(*eng, 2048);
    std::vector<sa::VoiceInfo> infos = eng->voice_infos();
    check(infos.size() == 1 && infos[0].glass > 0.95f, "A door's window counts, sa_debug shows it");
    eng->notify_channel_freed(0);
    sa::ChannelDescriptor next = line;
    next.mixer = 4;
    feed(*eng, 0, next, n, 4410); // no simulation step in between
    render(*eng, 8820 - 2048 + 512);
    infos = eng->voice_infos();
    bool kept = false;
    for (const sa::VoiceInfo& v : infos) kept = kept || (v.slot == 0 && v.glass > 0.95f);
    check(kept, "The next line starts behind the glass, too");
    // A level change: the next map's first line of an entity with the same number starts in the open, not behind the
    // pane of the map before.
    eng->flush_audio();
    eng->unload_scene();
    eng->load_scene(doors, summary);
    render(*eng, 4096);
    sa::ChannelDescriptor after = line;
    after.mixer = 5;
    feed(*eng, 0, after, n, 4410); // no simulation step in between
    render(*eng, 2048);
    infos = eng->voice_infos();
    bool fresh = false;
    for (const sa::VoiceInfo& v : infos) fresh = fresh || (v.slot == 0 && v.dialogue && v.glass < 0.05f);
    check(fresh, "... but not the first line after a level change");
    b.origin[1] = 300.0f; // the door slides away
    eng->update_brush_entities(&b, 1);
    feed(*eng, 0, next, n, 22050);
    run_ticks(*eng, 2);
    render(*eng, 22050);
    infos = eng->voice_infos();
    bool clear = !infos.empty();
    for (const sa::VoiceInfo& v : infos) clear = clear && (v.slot != 0 || v.glass < 0.05f);
    check(clear, "... and an open door lets it out");
}

void test_ambient_directions() {
    std::printf("=== Test 38: Street sounds come from the street ===\n");
    sa::ChannelSnapshot step{};
    step.name = "Area\\Special\\Tutorial\\Pedestrianby1.wav";
    step.entnum = 33;
    step.entchannel = 3;
    step.origin[1] = 400.0f;
    step.dist_mult = 0.002f;
    step.master_vol = 204;
    step.skip_collide = true;
    sa::ChannelDescriptor d = sa::describe_channel(step);
    check(d.free_ambient && d.outdoor_ambient && d.spatial_3d, "A random sound of a sound scheme is recognized");
    sa::ChannelSnapshot x = step;
    x.is_looped = true;
    check(!sa::describe_channel(x).free_ambient, "Loops stay where the map has them");
    x = step;
    x.skip_collide = false;
    check(!sa::describe_channel(x).free_ambient, "... and sounds of things in the world");
    x = step;
    x.name = "environmental/interior/ambient_high_heels3.wav";
    check(!sa::describe_channel(x).free_ambient, "... and what the game plays inside houses");
    sa::ChannelTweaks t;
    t.ambient_directions = false;
    check(!sa::describe_channel(step, sa::GainModel(), t).free_ambient, "fix_ambient_directions=0 is the original game");

    // A street along x, 20 m wide, between houses, under the sky; a dead end at x = 2000.
    sa::AcousticMeshData mesh;
    mesh.materials = sa::default_acoustic_materials();
    mesh.map_name = "canyon";
    add_box(mesh, -3000, 3000, -800, 800, -20, 0);
    add_box(mesh, -3000, 3000, 200, 600, 0, 600);
    add_box(mesh, -3000, 3000, -600, -200, 0, 600);
    add_box(mesh, 2000, 2100, -200, 200, 0, 600);
    const sa::AcousticBrushPlane sky[6] = {
        { 1, 0, 0, 3000, 0 }, { -1, 0, 0, 3000, 0 }, { 0, 1, 0, 1000, 0 }, { 0, -1, 0, 1000, 0 }, { 0, 0, 1, 1020, 0 }, { 0, 0, -1, -1000, 0, true },
    };
    sa::clip_sky_brush(sky, 6, mesh);
    sa::SkyProbe probe;
    probe.build(mesh);
    const float ear[3] = { 0, 0, 64 };
    const IPLVector3 listener = sa::source_pos_to_ipl(ear);
    const auto place = [&](float px, float py, float pz, float out[3]) {
        const float p[3] = { px, py, pz };
        const IPLVector3 v = probe.open_direction(listener, sa::source_pos_to_ipl(p));
        out[0] = -v.z / sa::kSourceToMeters;
        out[1] = -v.x / sa::kSourceToMeters;
        out[2] = v.y / sa::kSourceToMeters;
    };
    float p[3];
    place(0, 400, 30, p);
    std::printf("    400 inches into a house: %.0f %.0f %.0f\n", p[0], p[1], p[2]);
    check(std::fabs(p[1]) < 200.0f && std::sqrt(p[0] * p[0] + p[1] * p[1]) > 240.0f, "Out of the house, onto the pavement in front of it");
    check(p[1] > 100.0f && p[2] > 25.0f && p[2] < 60.0f, "... on the same side, above the ground");
    place(-350, 60, 30, p);
    check(std::fabs(p[0] + 350) < 1 && std::fabs(p[1] - 60) < 1 && std::fabs(p[2] - 30) < 1, "What is in the street already stays");
    place(300, 0, -400, p);
    check(p[2] > 400.0f, "An airplane below the ground flies above it");
    place(1200, 1200, 64, p);
    check(std::fabs(p[1]) < std::fabs(p[0]) * 0.75f, "A far siren comes down the street");
    bool ok = true;
    for (int k = 0; k < 360; k += 5) {
        const float a = static_cast<float>(k) * kPi / 180.0f;
        place(450.0f * std::cos(a), 450.0f * std::sin(a), 30, p);
        if (std::fabs(p[1]) > 201.0f || p[2] < 0.0f || !std::isfinite(p[0] + p[1] + p[2])) ok = false;
    }
    check(ok, "Whatever direction the game draws: never in a wall, never under the ground");
    {
        sa::AcousticMeshData cell;
        cell.materials = sa::default_acoustic_materials();
        add_box(cell, -50, 50, -50, -40, -50, 50);
        add_box(cell, -50, 50, 40, 50, -50, 50);
        add_box(cell, -50, -40, -50, 50, -50, 50);
        add_box(cell, 40, 50, -50, 50, -50, 50);
        sa::SkyProbe boxed;
        boxed.build(cell);
        const IPLVector3 src = sa::source_pos_to_ipl(400.0f, 0.0f, 0.0f);
        const IPLVector3 got = boxed.open_direction(sa::source_pos_to_ipl(0.0f, 0.0f, 0.0f), src);
        check(got.x == src.x && got.y == src.y && got.z == src.z, "Boxed in: nothing changes");
        sa::SkyProbe empty;
        const IPLVector3 same = empty.open_direction(sa::source_pos_to_ipl(0.0f, 0.0f, 0.0f), src);
        check(same.x == src.x && same.z == src.z, "No map: nothing changes");
    }

    // Through the engine. Facing +x, the steps were drawn to the left, inside the house (ear to ear), further up the street.
    const auto play = [&](const sa::AcousticMeshData& m, bool free_ambient, float yaw, bool& moved, double& level) {
        auto eng = make_engine();
        std::string summary;
        eng->load_scene(m, summary);
        Camera cam;
        cam.org[2] = 64.0f;
        cam.yaw(yaw);
        publish(*eng, cam);
        run_ticks(*eng, 1);
        sa::ChannelDescriptor s = source_at(0, 400, 30, 0.002f);
        s.no_occlusion = s.outdoor_ambient = true;
        s.free_ambient = free_ambient;
        Noise n;
        feed(*eng, 0, s, n, 8820);
        run_ticks(*eng, 1);
        const Spectrum e = measure(*eng, 4410);
        const std::vector<sa::VoiceInfo> infos = eng->voice_infos();
        moved = infos.size() == 1 && infos[0].relocated;
        level = infos.empty() ? 0.0 : infos[0].attenuation;
        return db_of(e.left / e.right);
    };
    bool moved = false, before_moved = false;
    double level = 0.0, before_level = 0.0;
    const double before = play(mesh, false, 0.0f, before_moved, before_level), after = play(mesh, true, 0.0f, moved, level);
    std::printf("    left over right: %.1f dB -> %.1f dB, level %.3f -> %.3f\n", before, after, before_level, level);
    check(!before_moved && moved, "The sound is moved, and sa_debug says so");
    check(std::fabs(level - before_level) < 1e-4, "... at the loudness the game gave it");
    check(std::fabs(after - before) > 2.0, "... and heard from there");
    sa::AcousticMeshData roofed = mesh;
    roofed.sky_vertices.clear();
    roofed.sky_triangles.clear();
    add_box(roofed, -3000, 3000, -800, 800, 600, 620);
    play(roofed, true, 0.0f, moved, level);
    check(!moved, "Maps without sky keep their sounds");
    sa::AcousticMeshData hall = street_mesh();
    auto eng = make_engine();
    std::string summary;
    eng->load_scene(hall, summary);
    Camera cam;
    cam.org[0] = 700.0f;
    cam.org[2] = 64.0f;
    publish(*eng, cam);
    run_ticks(*eng, 1);
    sa::ChannelDescriptor s = source_at(700, 500, 64, 0.002f);
    s.no_occlusion = s.outdoor_ambient = s.free_ambient = true;
    Noise n;
    feed(*eng, 0, s, n, 8820);
    run_ticks(*eng, 1);
    render(*eng, 2048);
    std::vector<sa::VoiceInfo> infos = eng->voice_infos();
    check(infos.size() == 1 && !infos[0].relocated, "Inside a house nothing is moved");
    cam.org[0] = -400.0f; // the player steps out while it plays
    publish(*eng, cam);
    run_ticks(*eng, 3);
    render(*eng, 1024);
    infos = eng->voice_infos();
    check(infos.size() == 1 && !infos[0].relocated, "... not even half-way through a sound");
}

// With --maps <the game's maps folder>, read only.
// ---- Voices at one loudness ----

// Something like speech: syllables of a buzzing, hissing voice, four a second, with a pause after each second of them.
// As loud as `meter_db` says, by SpeechMeter.
std::vector<float> speech(float meter_db, double seconds, float rate = 44100.0f, double lead = 0.0, double tail = 0.0,
                          float room_db = -75.0f, unsigned seed = 1) {
    const std::size_t n = static_cast<std::size_t>(seconds * rate), a = static_cast<std::size_t>(lead * rate),
                      b = static_cast<std::size_t>(tail * rate);
    std::vector<float> x(a + n + b, 0.0f);
    unsigned state = seed * 2654435761u + 1u;
    const auto noise = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    };
    float z = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double in_phrase = std::fmod(t, 1.35);
        const double syllable = std::sin(3.14159265 * std::fmod(in_phrase * 4.0, 1.0));
        const double env = in_phrase < 1.0 ? syllable * syllable * (0.8 + 0.2 * std::sin(7.0 * t)) : 0.0;
        double v = 0.0;
        for (int h = 1; h <= 18; ++h) v += std::sin(6.283185307 * 150.0 * h * t) / h;
        z += (noise() - z) * 0.5f;
        x[a + i] = static_cast<float>(env * (v + 0.4 * z));
    }
    sa::SpeechMeter m;
    m.add(x.data(), nullptr, static_cast<int>(x.size()), rate);
    const float g = std::pow(10.0f, (meter_db - m.db()) / 20.0f), room = std::pow(10.0f, room_db / 20.0f);
    for (float& s : x) s = s * g + room * noise();
    return x;
}

float speech_db(const std::vector<float>& x, float rate = 44100.0f) {
    sa::SpeechMeter m;
    m.add(x.data(), nullptr, static_cast<int>(x.size()), rate);
    return m.db();
}

struct Leveled {
    std::vector<float> out;
    float out_db = 0.0f, peak = 0.0f;
    float most = -99.0f, least = 99.0f; // gain, dB
    float biggest_step = 0.0f;          // of the gain (not dB) between two blocks, once speech is heard
    float db = 0.0f, seconds = 0.0f;
    std::vector<float> gains;           // one per block of 128
    bool  finite = true;
};

// One line through the leveler, as the audio thread does it.
Leveled level_line(const std::vector<float>& x, const sa::VoiceLevelSettings& s, float known_db = sa::kVoiceTargetDb,
                   float known_seconds = 0.1f, float ahead_seconds = sa::VoiceLeveler::kLookAheadSeconds) {
    const float rate = 44100.0f;
    Leveled r;
    r.out = x;
    sa::VoiceLeveler lev;
    lev.reset(known_db, known_seconds);
    std::size_t heard = 0;
    const std::size_t ahead = static_cast<std::size_t>(ahead_seconds * rate);
    for (std::size_t i = 0; i + 128 <= x.size(); i += 128) {
        const std::size_t to = std::min(x.size(), i + 128 + ahead);
        lev.listen(x.data() + heard, nullptr, static_cast<int>(to - heard), rate);
        heard = to;
        lev.process(r.out.data() + i, nullptr, 128, rate, s, static_cast<float>(heard - i - 128) / rate, true);
        const float g = lev.gain_db();
        r.gains.push_back(g);
        r.most = std::max(r.most, g);
        r.least = std::min(r.least, g);
    }
    for (std::size_t i = 40; i < r.gains.size(); ++i) {
        r.biggest_step = std::max(r.biggest_step, std::fabs(std::pow(10.0f, r.gains[i] / 20.0f) - std::pow(10.0f, r.gains[i - 1] / 20.0f)));
    }
    r.out_db = speech_db(r.out);
    for (float f : r.out) {
        r.peak = std::max(r.peak, std::fabs(f));
        r.finite = r.finite && std::isfinite(f);
    }
    r.db = lev.db();
    r.seconds = lev.seconds();
    return r;
}

// Plays a line through the engine and measures what comes out.
struct Heard {
    float db = 0.0f, peak = 0.0f, gain_db = 0.0f, recorded_db = -120.0f;
    bool  finite = true;
};
Heard play_line(sa::SteamAudioEngine& eng, const sa::ChannelDescriptor& d, const std::vector<float>& x) {
    Heard h;
    const int rate = eng.output_rate(), out_block = rate / 100;
    std::vector<float> mono, buf(static_cast<std::size_t>(out_block) * 2);
    std::size_t fed = 0, steps = (x.size() + 22050) / 441;
    for (std::size_t step = 0; step < steps; ++step) {
        // engine.dll mixes a fifth of a second ahead.
        const std::size_t blocks = step == 0 ? 20 : 1;
        for (std::size_t b = 0; b < blocks; ++b) {
            if (fed < x.size()) {
                const int n = static_cast<int>(std::min<std::size_t>(441, x.size() - fed));
                eng.begin_chunk(1020);
                eng.feed_channel_mono(0, d, x.data() + fed, 0, n);
                eng.end_chunk(n);
                fed += static_cast<std::size_t>(n);
                if (fed >= x.size()) eng.notify_channel_freed(0); // engine.dll frees it as soon as it is mixed
            } else {
                feed_silence(eng, 441);
            }
        }
        eng.render_output(buf.data(), out_block);
        for (int i = 0; i < out_block; ++i) {
            const float m = 0.5f * (buf[static_cast<std::size_t>(i) * 2] + buf[static_cast<std::size_t>(i) * 2 + 1]);
            mono.push_back(m);
            h.peak = std::max(h.peak, std::fabs(m));
            h.finite = h.finite && std::isfinite(m);
        }
        for (const sa::VoiceInfo& v : eng.voice_infos()) {
            if (v.recorded_db < -100.0f) continue;
            h.gain_db = v.level_db;
            h.recorded_db = v.recorded_db;
        }
    }
    h.db = speech_db(mono, static_cast<float>(rate));
    return h;
}

void test_voice_levels() {
    std::printf("=== Test 39: Everybody speaks at one loudness ===\n");
    const float target = sa::kVoiceTargetDb;

    // The meter: speech only.
    const std::vector<float> line = speech(-20.0f, 3.0), padded = speech(-20.0f, 3.0, 44100.0f, 1.0, 2.0, -58.0f);
    check(nearf(speech_db(line), -20.0f, 0.1f) && std::fabs(speech_db(padded) - speech_db(line)) < 0.5f,
          "Silence and room tone around a line do not count");
    std::vector<float> at48 = speech(-20.0f, 3.0, 48000.0f), sine(44100);
    for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = 0.1f * std::sin(6.2831853f * 997.0f * static_cast<float>(i) / 44100.0f);
    check(nearf(speech_db(at48, 48000.0f), -20.0f, 0.2f) && std::fabs(speech_db(sine) + 22.3f) < 0.5f,
          "... at any rate, as ITU-R BS.1770 weighs it");
    sa::SpeechMeter quiet;
    const std::vector<float> nothing(44100, 0.0f);
    quiet.add(nothing.data(), nullptr, 44100, 44100.0f);
    check(quiet.seconds() == 0.0f && !quiet.speech(), "Silence is no speech");

    // The recordings the player named, as loud as they are in the game (measured with this meter).
    struct Named { const char* what; float db; double seconds; const sa::VoiceLevelSettings* s; };
    const Named named[] = {
        { "\"Lovely evening!\"", -14.3f, 1.2, &sa::kBarkVoice },
        { "\"It's a nice night out, huh?\"", -17.8f, 2.4, &sa::kBarkVoice },
        { "\"Out of my way.\"", -16.0f, 0.6, &sa::kBarkVoice },
        { "prostitute's bark (HD Overhaul)", -14.8f, 2.3, &sa::kBarkVoice },
        { "\"Hey, baby.\" in a conversation", -14.9f, 2.2, &sa::kDialogueVoice },
        { "... to a woman", -11.7f, 2.7, &sa::kDialogueVoice },
    };
    for (const Named& n : named) {
        const std::vector<float> x = speech(n.db, n.seconds);
        const Leveled first = level_line(x, *n.s), again = level_line(x, *n.s, first.db, 8.0f * first.seconds);
        std::printf("    %-32s %.1f -> %.1f (%.1f the second time), gain %.1f to %.1f dB\n", n.what, n.db, first.out_db, again.out_db,
                    first.least, first.most);
        check(std::fabs(first.out_db - target) < 2.0f && std::fabs(again.out_db - target) < 1.2f && first.finite,
              "... comes down to everybody's loudness");
        check(again.most - again.least < 2.0f && first.peak < 0.95f, "... once it is known, all of it by about the same");
    }
    const std::vector<float> watch = speech(-26.1f, 0.9), faint = speech(-31.0f, 0.9);
    const Leveled w = level_line(watch, sa::kBarkVoice), w2 = level_line(watch, sa::kBarkVoice, w.db, 8.0f * w.seconds);
    const Leveled f = level_line(faint, sa::kBarkVoice, w.db, 0.4f), f2 = level_line(faint, sa::kBarkVoice, f.db, 8.0f * f.seconds);
    std::printf("    \"Buy a watch.\" -26.1 -> %.1f (%.1f the second time); at -31: %.1f (%.1f)\n", w.out_db, w2.out_db, f.out_db, f2.out_db);
    check(w.out_db > -23.8f && w.out_db < target && std::fabs(w2.out_db - target) < 1.0f, "\"Buy a watch.\" comes up");
    check(f.out_db > -27.0f && f2.out_db > -24.0f && f2.out_db < -22.5f && f2.most <= 8.0f, "What is fainter still comes up by 8 dB, no more");
    const Leveled normal = level_line(speech(target, 3.0), sa::kBarkVoice), near_it = level_line(speech(target - 1.0f, 3.0), sa::kDialogueVoice);
    std::printf("    normal voice: %.2f to %.2f dB; 1 dB below: %.2f to %.2f dB\n", normal.least, normal.most, near_it.least, near_it.most);
    check(normal.least > -2.0f && normal.most < 0.3f && std::fabs(normal.out_db - target) < 0.6f && near_it.least > -1.5f && near_it.most < 0.3f,
          "A normal voice stays what it is");

    // Conversations: a whisper, an outburst, a pause.
    const Leveled whisper = level_line(speech(-28.0f, 4.0), sa::kDialogueVoice, -20.0f, 0.4f);
    std::printf("    whisper in a conversation: -28.0 -> %.1f\n", whisper.out_db);
    check(whisper.out_db > -26.5f && whisper.out_db < -23.9f && whisper.most <= 4.0f, "A whisper is easier to hear, and still a whisper");
    const std::vector<float> talk = speech(-22.0f, 2.7), shout = speech(-10.0f, 1.0, 44100.0f, 0.0, 0.0, -75.0f, 7);
    std::vector<float> scene = talk;
    scene.insert(scene.end(), shout.begin(), shout.end());
    const Leveled drama = level_line(scene, sa::kDialogueVoice);
    const std::ptrdiff_t cut = static_cast<std::ptrdiff_t>(talk.size());
    const float calm_db = speech_db(std::vector<float>(drama.out.begin(), drama.out.begin() + cut));
    const float shout_db = speech_db(std::vector<float>(drama.out.begin() + cut, drama.out.end()));
    std::printf("    -22 and then a shout of -10: %.1f and %.1f\n", calm_db, shout_db);
    check(shout_db > calm_db + 2.5f && shout_db < -14.0f, "An outburst stays one, but not at full scale");

    // Pauses: the gain does not move in them, room tone is not pulled up.
    std::vector<float> paused = speech(-26.0f, 1.0, 44100.0f, 0.0, 1.5, -54.0f);
    const std::vector<float> second = speech(-26.0f, 1.0, 44100.0f, 0.0, 0.0, -54.0f, 3);
    paused.insert(paused.end(), second.begin(), second.end());
    const Leveled p = level_line(paused, sa::kBarkVoice);
    float lo = 99.0f, hi = -99.0f;
    for (std::size_t i = 48000 / 128; i < 98000 / 128; ++i) {
        lo = std::min(lo, p.gains[i]);
        hi = std::max(hi, p.gains[i]);
    }
    std::printf("    gain in a pause: %.2f to %.2f dB\n", lo, hi);
    check(hi - lo < 0.2f && hi > 1.0f, "No pumping in a pause");
    std::vector<float> hiss(44100 * 2);
    Noise n;
    n.next(hiss.data(), static_cast<int>(hiss.size()));
    for (float& s : hiss) s *= std::pow(10.0f, -62.0f / 20.0f) / 0.14434f;
    std::vector<float> tone = hiss;
    for (float& s : tone) s *= std::pow(10.0f, 14.0f / 20.0f); // louder room tone, nobody's voice
    check(level_line(hiss, sa::kBarkVoice).out == hiss && level_line(nothing, sa::kBarkVoice).out == nothing, "Hiss and silence pass bit for bit");
    check(level_line(tone, sa::kBarkVoice).most < 1.0f, "Room tone is not taken for a quiet voice");
    const Leveled tail = level_line(speech(-20.0f, 1.0, 44100.0f, 0.0, 2.0, -45.0f), sa::kBarkVoice);
    check(tail.gains.back() < 0.5f, "The hiss at the end of a line is not pulled up");

    // Peaks: a quiet line with a click at -2 dBFS is not raised into clipping.
    std::vector<float> clicky = speech(-28.0f, 2.0);
    for (std::size_t i = 0; i < 40; ++i) clicky[40000u + i] = (i & 1u) ? 0.79f : -0.79f;
    const Leveled c = level_line(clicky, sa::kBarkVoice, -28.0f, 4.0f);
    std::printf("    quiet line with a peak of 0.79: peak %.2f\n", c.peak);
    check(c.peak <= 0.83f && c.out_db > -24.0f, "A raised line never clips");
    const Leveled late = level_line(speech(-14.0f, 2.0), sa::kBarkVoice, target, 0.1f, 0.0f);
    check(std::fabs(late.out_db - target) < 2.0f && late.finite, "It works without hearing ahead, too (after a hitch)");
    const Leveled cry = level_line(speech(-10.0f, 1.0), sa::kCryVoice), moan = level_line(speech(-30.0f, 1.0), sa::kCryVoice);
    std::printf("    scream of -10: %.1f\n", cry.out_db);
    check(cry.out_db > -16.5f && cry.out_db < -11.5f && moan.most <= 0.0f, "Screams stay screams, moans are not raised");
    {
        // No click, whatever comes: the gain's steps are ramps over a block, 0.006 of it are 5e-5 per sample.
        std::vector<float> x = speech(-34.0f, 1.5);
        const std::vector<float> loud = speech(-8.0f, 1.0);
        x.insert(x.end(), loud.begin(), loud.end());
        const Leveled wild = level_line(x, sa::kBarkVoice, -34.0f, 2.0f);
        std::printf("    from -34 to -8 in a word: largest gain step per block %.4f, peak %.2f\n", wild.biggest_step, wild.peak);
        check(wild.biggest_step < 0.06f && wild.peak < 0.98f && wild.finite, "The gain moves in ramps, never in steps");
        sa::VoiceLeveler lev;
        lev.reset(target, 0.1f);
        std::vector<float> fine(128, 0.1f), bad(512, std::nanf(""));
        lev.listen(x.data(), nullptr, 22050, 44100.0f);
        lev.listen(bad.data(), nullptr, 512, 44100.0f);
        lev.process(fine.data(), nullptr, 128, 44100.0f, sa::kBarkVoice, 0.1f, true);
        check(std::isfinite(lev.gain_db()) && std::isfinite(lev.db()) && std::isfinite(fine[127]), "Broken samples are ignored");
        for (int b = 0; b < 400; ++b) lev.process(fine.data(), nullptr, 128, 44100.0f, sa::kBarkVoice, 0.1f, false);
        std::vector<float> plain(128, 0.25f);
        lev.process(plain.data(), nullptr, 128, 44100.0f, sa::kBarkVoice, 0.1f, false);
        check(lev.gain_db() == 0.0f && plain[100] == 0.25f, "Turned off, it goes back to the recording");
        float l[128], r[128];
        for (int i = 0; i < 128; ++i) {
            l[i] = 0.4f;
            r[i] = 0.1f;
        }
        sa::VoiceLeveler st;
        st.reset(-10.0f, 5.0f);
        st.process(l, r, 128, 44100.0f, sa::kBarkVoice, 0.0f, true);
        check(nearf(l[64], 4.0f * r[64], 1e-5f) && l[64] < 0.2f, "Both sides of a stereo line get the same gain");
    }
    {
        auto mem = std::make_unique<sa::VoiceMemory>();
        float db = 0.0f, s = 0.0f;
        mem->recall(11, 5, db, s);
        check(db == target && s < 0.2f, "Who is not known may be anything");
        mem->remember(11, 5, -14.0f, 1.0f);
        mem->recall(11, 5, db, s);
        check(nearf(db, -14.0f) && s > 5.0f, "A file that was heard is known");
        mem->recall(12, 5, db, s);
        check(nearf(db, -14.0f) && s < 1.0f, "... and what else its speaker says is likely to be like it");
        mem->remember(11, 5, -30.0f, 0.3f);
        mem->recall(11, 5, db, s);
        check(nearf(db, -14.0f), "A line that was cut off does not replace the whole one");
        mem->remember(13, 5, std::nanf(""), 1.0f);
        mem->recall(13, 6, db, s);
        check(db == target, "Nonsense is not remembered");
        mem->clear();
        mem->recall(11, 5, db, s);
        check(db == target, "... and everything can be forgotten");
    }

    // Which sounds.
    sa::ChannelSnapshot v = bark_snapshot();
    sa::ChannelDescriptor d = sa::describe_channel(v);
    check(d.leveling == sa::Leveling::kBark && d.voice_file != 0 && d.voice_speaker != 0, "The prostitutes' barks are leveled");
    sa::ChannelSnapshot same_file = v, sister = v;
    same_file.name = "*Character/Female/hooker2/FLOAT_3.wav";
    sister.name = "character/female/hooker2/float_4.wav";
    check(sa::describe_channel(same_file).voice_file == d.voice_file && sa::describe_channel(sister).voice_file != d.voice_file &&
          sa::describe_channel(sister).voice_speaker == d.voice_speaker, "A file is known by its name, a speaker by the folder");
    v.name = "character/female/hooker2/pain_2.wav";
    check(sa::describe_channel(v).leveling == sa::Leveling::kCry && sa::is_cry_sound("character\\male\\scream_death1.wav") &&
          !sa::is_cry_sound("character/male/target_lost_3.wav") && !sa::is_cry_sound("character/dlg/x/painful.mp3"), "Cries of pain are no speech");
    sa::ChannelSnapshot talk_line{};
    talk_line.name = "*character\\dlg\\generic\\prostitute\\line1_col_e.mp3";
    talk_line.entnum = 212;
    talk_line.entchannel = sa::kChanStream;
    talk_line.dist_mult = dist_mult_for(80, 50); // CSceneEntity gives every conversation 80 dB, whatever its .vcd says
    talk_line.master_vol = 255;
    talk_line.is_voice = talk_line.skip_collide = true;
    talk_line.origin[0] = talk_line.distance = 60.0f;
    d = sa::describe_channel(talk_line);
    check(d.dialogue && d.spatial_3d && d.leveling == sa::Leveling::kDialogue && nearf(d.dist_mult, talk_line.dist_mult, 1e-9f),
          "Her line in a conversation is leveled, at its place");
    talk_line.dist_mult = 0.0f;
    check(sa::describe_channel(talk_line).leveling == sa::Leveling::kDialogue && !sa::describe_channel(talk_line).spatial_3d,
          "... also those that play everywhere (\"full_sound\")");
    sa::ChannelTweaks off;
    off.level_voices = false;
    check(sa::describe_channel(talk_line, sa::GainModel(), off).leveling == sa::Leveling::kNone &&
          sa::describe_channel(bark_snapshot(), sa::GainModel(), off).leveling == sa::Leveling::kNone, "level_voices=0: nothing is");
    for (const char* name : { "music/santa_monica/theme.mp3", "surfaces/concrete/stepleft1.wav", "player/footsteps/concrete1.wav",
                              "weapons/ranged/glock/fire.wav", "interface/click.wav", "area/santa_monica/thunder_heavy1.wav",
                              "environmental/city/car by 2.wav", "character/monster/zombie/roar.wav", "radio/deb1.mp3", "whispers/danger/a.wav" }) {
        sa::ChannelSnapshot o{};
        o.name = name;
        o.entnum = 40;
        o.entchannel = sa::kChanVoice;
        o.dist_mult = 0.002f;
        o.master_vol = 255;
        const sa::Leveling world = sa::describe_channel(o).leveling;
        o.dist_mult = 0.0f;
        check(world == sa::Leveling::kNone && sa::describe_channel(o).leveling == sa::Leveling::kNone, name);
    }

    // Through the engine: a hot, a normal and a faint line from the same place, in the street and in a conversation.
    for (const sa::Leveling kind : { sa::Leveling::kBark, sa::Leveling::kDialogue }) {
        auto eng = make_engine();
        Camera cam;
        publish(*eng, cam);
        const auto heard = [&](float db, sa::Leveling leveling, unsigned file) {
            sa::ChannelDescriptor s = source_at(60.0f, 0.0f, 0.0f, dist_mult_for(75, 50));
            s.is_voice = true;
            s.leveling = leveling;
            s.voice_file = file;
            s.mixer = file;
            return play_line(*eng, s, speech(db, 2.5, 44100.0f, 0.05, 0.1));
        };
        const Heard raw_hot = heard(-14.0f, sa::Leveling::kNone, 101), raw_normal = heard(-20.0f, sa::Leveling::kNone, 102);
        const Heard hot = heard(-14.0f, kind, 103), ordinary = heard(-20.0f, kind, 104), low = heard(-27.0f, kind, 105);
        const Heard hot_again = heard(-14.0f, kind, 103);
        std::printf("    %s: hot %.1f -> %.1f (again %.1f), normal %.1f -> %.1f, faint -> %.1f\n", kind == sa::Leveling::kBark ? "street" : "conversation",
                    raw_hot.db, hot.db, hot_again.db, raw_normal.db, ordinary.db, low.db);
        check(raw_hot.db > raw_normal.db + 5.0f && std::fabs(hot.db - ordinary.db) < 1.8f && std::fabs(hot_again.db - ordinary.db) < 1.4f,
              "The hot line is as loud as the normal one");
        check(low.db > ordinary.db - (kind == sa::Leveling::kBark ? 3.2f : 6.3f) && low.db < ordinary.db, "... the faint one more so than it was");
        check(std::fabs(ordinary.db - raw_normal.db) < 1.2f, "... and the normal one what it was");
        check(hot.finite && low.finite && hot.peak < 0.9f && hot.gain_db < -3.0f && nearf(hot.recorded_db, -14.0f, 1.0f), "sa_debug says what happens");
        check(raw_normal.recorded_db < -100.0f, "... and nothing about what is not leveled");
    }
}

void test_step_ambients() {
    std::printf("=== Test 40: Heels and hoofs of nobody ===\n");
    const auto gain = [](const char* name, bool scheme, const sa::ChannelTweaks& t) {
        sa::ChannelSnapshot c{};
        c.name = name;
        c.entnum = 33;
        c.entchannel = 3;
        c.origin[1] = 400.0f;
        c.dist_mult = 0.002f;
        c.master_vol = 204;
        c.skip_collide = scheme;
        return sa::describe_channel(c, sa::GainModel(), t).master_gain;
    };
    const sa::ChannelTweaks on;
    sa::ChannelTweaks heels_only, hoofs_only, off;
    heels_only.no_hoofs = false;
    hoofs_only.no_heels = false;
    off.no_heels = off.no_hoofs = false;
    for (const char* name : { "Area\\Special\\Tutorial\\Pedestrianby1.wav", "area/special/tutorial/pedestrianby2.wav",
                              ")Area/Special/Tutorial/Pedestrianby3.wav", "area/special/tutorial/pedestrianby5.wav",
                              "Environmental/Interior/Ambient_High_Heels.wav", "environmental/interior/ambient_high_heels2.wav",
                              "Environmental/Interior/Ambient_High_Heels3.wav" }) {
        check(gain(name, true, on) == 0.0f && gain(name, true, heels_only) == 0.0f && gain(name, true, hoofs_only) > 0.7f &&
              gain(name, true, off) > 0.7f, name);
    }
    const char* hoof = "Area\\Special\\Tutorial\\Pedestrianby4.wav";
    check(gain(hoof, true, on) == 0.0f && gain(hoof, true, hoofs_only) == 0.0f && gain(hoof, true, heels_only) > 0.7f && gain(hoof, true, off) > 0.7f,
          "The hoofs have a switch of their own");
    for (const char* name : { "surfaces/concrete/stepleft1.wav", "surfaces/stepright4.wav", "surfaces/metalgrate/metalgrate_stepleft1.wav",
                              "player/footsteps/concrete1.wav", "physics/concrete/step1.wav", "surfaces/heels/stepleft1.wav",
                              "player/footsteps/heels2.wav", "character/monster/spiderchick/spi_footstep_indiv_1.wav",
                              "area/santa_monica/ocean_house/ambient_walk.wav", "area/santa_monica/clinic/squeaky_shoes.wav",
                              "environmental/city/industry_train_wheels1.wav", "area/hollywood/warrens/waterwheel.wav",
                              "environmental/city/car by 2.wav" }) {
        check(gain(name, true, on) > 0.7f && gain(name, false, on) > 0.7f, name);
    }
    check(gain("Environmental/Interior/Ambient_High_Heels3.wav", false, on) > 0.7f, "The ghost's heels in the Ocean House are a scene, not a background");
    sa::ChannelSnapshot own{};
    own.name = "area/special/tutorial/pedestrianby1.wav";
    own.own = own.player = own.skip_collide = true;
    own.volumes[0] = own.volumes[1] = 127;
    check(sa::describe_channel(own).master_gain > 0.7f, "Nothing the player does is ever left out");

    // Through the engine: nothing is heard.
    const auto loudness = [&](const sa::ChannelTweaks& t) {
        auto eng = make_engine();
        Camera cam;
        publish(*eng, cam);
        sa::ChannelSnapshot c{};
        c.name = "area/special/tutorial/pedestrianby1.wav";
        c.entnum = 33;
        c.entchannel = 3;
        c.origin[1] = -120.0f;
        c.dist_mult = 0.002f;
        c.master_vol = 204;
        c.skip_collide = true;
        sa::ChannelDescriptor s = sa::describe_channel(c, sa::GainModel(), t);
        s.mixer = 3;
        LoudNoise steps(-25.0f);
        const Spectrum e = settle(*eng, 1, s, steps, 0.2);
        return e.left + e.right;
    };
    check(loudness(on) == 0.0 && loudness(off) > 1e-7, "Left out is silence");
}

// Found in the pre-release audit; each check failed before its fix.
void test_audit_fixes() {
    std::printf("=== Test 42: Pre-release audit ===\n");
    // "sa_volume nan" (std::atof parses it) made the master gain NaN for good: silence that no later sa_volume undid.
    {
        auto eng = make_engine();
        publish(*eng, Camera());
        sa::ChannelDescriptor ui = source_at(0, 0, 0);
        ui.spatial_3d = false;
        Tone tone;
        feed(*eng, 0, ui, tone, 8820);
        render(*eng, 4096);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        eng->set_master_volume(0.8f);
        eng->set_master_volume(nan);
        eng->set_reverb_wet(nan);
        eng->set_weather_volume(nan);
        eng->set_engine_volume(nan);
        const sa::EngineConfig c = eng->config();
        check(c.master_volume == 0.8f && c.reverb_wet == 0.55f && c.weather_volume == 0.65f,
              "A level that is no number changes nothing (sa_volume, sa_reverb, sa_weather_vol)");
        feed(*eng, 0, ui, tone, 8820);
        float peak = 0.0f;
        const Energy e = render(*eng, 4096, &peak);
        check(std::isfinite(e.l + e.r) && e.l > 1.0 && peak > 0.05f, "... and the sound plays on");
        eng->set_master_volume(std::numeric_limits<float>::infinity());
        check(eng->config().master_volume == 0.8f, "... nor does an infinite one");
    }
    // A malformed .bsp (the file fallback) with a NaN plane: base windings of NaN made NaN triangles for Steam Audio.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const sa::AcousticBrushPlane planes[8] = {
            {  1, 0, 0, 64, sa::kMatConcrete }, { -1, 0, 0, 64, sa::kMatConcrete },
            {  0, 1, 0, 64, sa::kMatConcrete }, { 0, -1, 0, 64, sa::kMatConcrete },
            {  0, 0, 1, 64, sa::kMatConcrete }, { 0, 0, -1, 64, sa::kMatConcrete },
            {  0, 0, 1, nan, sa::kMatConcrete }, { nan, 0, 1, 10, sa::kMatConcrete },
        };
        sa::AcousticMeshData mesh;
        sa::clip_brush_to_acoustic_mesh(planes, 8, mesh);
        bool finite = true;
        for (const IPLVector3& v : mesh.vertices) finite = finite && std::isfinite(v.x + v.y + v.z);
        check(mesh.triangles.size() == 12 && finite, "Planes that are no numbers bound nothing: a box stays a box");
    }
}

void test_real_maps() {
    std::printf("=== Test 41: The game's own maps (skipped without --maps) ===\n");
    if (g_maps_dir.empty()) return;
    const auto load = [&](const char* name, sa::AcousticMeshData& mesh) {
        std::string summary;
        const bool ok = sa::extract_acoustic_mesh_from_bsp_file((sa::narrow(g_maps_dir) + "\\" + name + ".bsp").c_str(), mesh, summary);
        std::printf("    %s, %u glass tris\n", summary.c_str(), static_cast<unsigned>(mesh.glass_triangles.size()));
        return ok;
    };
    const auto at = [](float x, float y, float z) { return sa::source_pos_to_ipl(x, y, z); };
    sa::AcousticMeshData mesh;
    sa::GlassProbe glass;
    check(load("sm_pawnshop_2", mesh), "The pawn shop loads");
    glass.build(mesh);
    const IPLVector3 trip = at(-2060, -2624, -110.8f + 0.88f * 72.0f);
    check(glass.between(at(-2060, -2584, -45.8f), trip, nullptr, 0) == 1.0f, "Trip is behind glass for the dialogue camera (40 units in front of him)");
    check(glass.between(at(-2058, -2540, -47.0f), trip, nullptr, 0) == 1.0f && glass.between(at(-2000, -2520, -47.0f), trip, nullptr, 0) == 1.0f,
          "... and for the player");
    check(glass.between(at(-2060, -2640, -47.0f), trip, nullptr, 0) == 0.0f, "... not on his side of the counter");
    check(load("sm_medical_1", mesh), "The clinic loads");
    glass.build(mesh);
    const IPLVector3 vandal = at(1480.1f, 385.0f, -327.0f + 0.88f * 72.0f);
    check(glass.between(at(1520.0f, 382.2f, -262.0f), vandal, nullptr, 0) == 1.0f, "Vandal is behind glass for the dialogue camera");
    check(glass.between(at(1560, 385, -263), vandal, nullptr, 0) == 1.0f, "... and for the player");
    check(glass.between(at(1440, 385, -263), vandal, nullptr, 0) == 0.0f, "... not on his side of the window");
    const IPLVector3 nurse = at(1233, 212, 2 + 0.88f * 72.0f);
    check(glass.between(at(1233, 172, 65), nurse, nullptr, 0) == 0.0f && glass.between(at(1273, 212, 65), nurse, nullptr, 0) == 0.0f,
          "The nurse at the reception desk is not muffled");

    check(load("sm_hub_1", mesh), "Santa Monica loads");
    sa::SkyProbe sky;
    sky.build(mesh);
    int inside = 0, inside_after = 0, total = 0;
    // Where people stand: around the prophet, the prostitutes, the asylum's door.
    const float spots[][3] = { { -1420, 1554, -47 }, { -1277, -60, -47 }, { -1300, 600, -47 } };
    for (const auto& spot : spots) {
        const IPLVector3 ear = at(spot[0], spot[1], spot[2]);
        for (int k = 0; k < 360; k += 10) {
            for (float r = 300.0f; r <= 500.0f; r += 100.0f) { // Pedestrianby1..5
                const float a = static_cast<float>(k) * kPi / 180.0f;
                const IPLVector3 src = at(spot[0] + r * std::cos(a), spot[1] + r * std::sin(a), -81);
                const IPLVector3 got = sky.open_direction(ear, src);
                ++total;
                if (sky.blocked(ear, src)) ++inside;
                if (sky.blocked(ear, got)) ++inside_after;
            }
        }
    }
    std::printf("    steps drawn around three places: %d of %d behind walls, %d afterwards\n", inside, total, inside_after);
    check(inside > total / 10 && inside_after <= total / 50, "Steps no longer come out of houses");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::wcscmp(argv[i], L"--sofa") == 0) g_sofa_file = argv[i + 1];
        if (std::wcscmp(argv[i], L"--maps") == 0) g_maps_dir = argv[i + 1];
        if (std::wcscmp(argv[i], L"--write-ini") == 0) { // the settings file that ships
            std::string err;
            const bool ok = sa::write_file_atomic(argv[i + 1], sa::format_ini(sa::EngineConfig{}), err);
            std::printf("%s\n", ok ? "written" : err.c_str());
            return ok ? 0 : 1;
        }
    }
    test_coords();
    test_lockfree();
    test_bsp_clipping_and_materials();
    test_steam_audio_live_pipeline();
    test_camera_turn_latency();
    test_source_moves_live();
    test_loop_cull_fade();
    test_voice_lifecycle();
    test_stream_integrity();
    test_hitches();
    test_flush_and_toggle();
    test_threads();
    test_cost();
    test_transforms();
    test_bsp_submodels();
    test_dynamic_doors();
    test_output_rate();
    test_resampler();
    test_reverb_modes();
    test_dialogue();
    test_channel_model();
    test_weather();
    test_device();
    test_focus();
    test_pacer();
    test_device_change();
    test_sofa();
    test_directivity();
    test_settings();
    test_buried_sources();
    test_dirty_memory();
    test_device_thread();
    test_barks();
    test_dynamics();
    test_loud_sounds();
    test_rain_masking();
    test_dialogue_glass();
    test_ambient_directions();
    test_voice_levels();
    test_step_ambients();
    test_audit_fixes();
    test_real_maps();
    std::printf("\n=== Summary: %d checks, %d failure(s) ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
