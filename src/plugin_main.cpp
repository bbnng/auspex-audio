#include "blse_api.h"
#include "bsp_acoustics.h"
#include "engine_hooks.h"
#include "settings.h"
#include "steam_audio_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

BLSE_PLUGIN_INFO(
    "auspex_audio",
    "1.0.1",
    "bigbannng",
    "Auspex Audio - Steam Audio + miniaudio 3D spatialization, HRTF, and BSP raytraced acoustics",
    "0.7",
    NULL);

namespace {

constexpr char kModName[] = "auspex_audio";

const BLSE_HostAPI* g_host = nullptr;
int g_split_role = BLSE_SPLIT_NONE;
std::string g_current_map;
std::wstring g_mod_dir; // <mod>: this DLL is <mod>\blse\plugins\auspex_audio.dll

// Created in BLSE_PluginLoad and deliberately never destroyed in the game: a static object's destructor would run in
// DllMain(DLL_PROCESS_DETACH) and join the raytracing thread and the WASAPI thread while Windows holds the loader
// lock. Those threads cannot finish without that lock (or were already killed by ExitProcess), so Vampire.exe never
// exited and kept this DLL locked. Windows ends the threads and takes the memory back at process exit by itself.
sa::SteamAudioEngine* g_audio = nullptr;

void logf(const char* fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(buf);
}

void con_printf(const char* fmt, ...) {
    if (!g_host || !BLSE_HOST_HAS(g_host, console_print)) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->console_print(g_host->context, buf);
}

bool payload_field(const void* payload, size_t size, int index, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!payload && size > 0) return false;
    const auto* p = static_cast<const char*>(payload);
    size_t start = 0;
    for (int f = 0; f < index; ++f) {
        while (start < size && p[start] != '\0') ++start;
        if (start >= size) return false;
        ++start;
    }
    size_t end = start;
    while (end < size && p[end] != '\0') ++end;
    size_t len = end - start;
    if (len >= cap) len = cap - 1;
    if (len > 0) std::memcpy(out, p + start, len);
    out[len] = '\0';
    return true;
}

sa::EngineConfig load_config() {
    if (!g_host || !BLSE_HOST_HAS(g_host, ini_get)) return sa::EngineConfig{};
    sa::EngineConfig cfg = sa::read_config([](const char* section, const char* key, std::string& value) {
        char buf[520];
        size_t n = 0;
        if (!g_host->ini_get(g_host->context, kModName, section, key, buf, sizeof(buf), &n) || n == 0) return false;
        buf[sizeof(buf) - 1] = '\0';
        value = buf;
        return true;
    });
    if (!cfg.sofa_file.empty()) sa::resolve_sofa(g_mod_dir, cfg.sofa_file, cfg.sofa_path);
    return cfg;
}

const char* on_off(bool on) { return on ? "on" : "off"; }

bool load_bsp_acoustics_for_current_map(const char* map_hint, std::string& out_summary) {
    sa::AcousticMeshData mesh;
    std::string bsp_summary;
    bool ok = sa::extract_acoustic_mesh_from_engine(mesh, bsp_summary);
    if (!ok && map_hint && *map_hint) {
        std::string file_summary;
        ok = sa::extract_acoustic_mesh_for_map_name(map_hint, mesh, file_summary);
        bsp_summary += " | fallback: " + file_summary;
    }
    if (!ok) {
        out_summary = bsp_summary;
        return false;
    }
    std::string scene_summary;
    if (!g_audio->load_scene(mesh, scene_summary)) {
        out_summary = bsp_summary + " | " + scene_summary;
        return false;
    }
    out_summary = scene_summary;
    return true;
}

// ---- BLSE lifecycle callbacks ----

void BLSE_CALL on_level_init(const char*, const void* payload, size_t size, void*) {
    char map[128] = {};
    payload_field(payload, size, 0, map, sizeof(map));
    g_current_map = map;
    sa::reset_brush_tracking();

    std::string summary;
    if (load_bsp_acoustics_for_current_map(map, summary)) {
        logf("auspex_audio: %s", summary.c_str());
    } else {
        logf("auspex_audio: BSP acoustics unavailable for '%s' (%s); HRTF direct spatialization still active.",
             map, summary.c_str());
    }
}

void BLSE_CALL on_level_shutdown(const char*, const void*, size_t, void*) {
    g_current_map.clear();
    sa::reset_brush_tracking();
    g_audio->unload_scene();
    g_audio->flush_audio();
}

void BLSE_CALL on_engine_restart(const char*, const void*, size_t, void*) {
    g_current_map.clear();
    sa::reset_brush_tracking();
    g_audio->unload_scene();
    g_audio->flush_audio();
    if (!sa::reinstall_engine_sound_hooks(g_host)) {
        logf("auspex_audio: the restarted engine cannot be hooked; the game plays its own sound from here on.");
        return;
    }
    if (g_audio->is_enabled()) sa::clear_vanilla_audio_buffer();
}

// ---- Console commands ----

void BLSE_CALL cmd_status(int, const char* const*, const char*, void*) {
    const sa::EngineStats s = g_audio->get_stats();
    const sa::EngineConfig c = g_audio->config();
    const auto ms_at = [](uint32_t frames, uint32_t rate) {
        return rate > 0 ? 1000.0f * static_cast<float>(frames) / static_cast<float>(rate) : 0.0f;
    };
    const auto dry_ms = [&](uint32_t frames) { return ms_at(frames, sa::kDryRate); };
    con_printf(
        "auspex_audio: enabled=%d hooks=%d wasapi=%d hrtf=%d occ=%d reverb=%d(wet=%.2f) vol=%.2f\n",
        s.enabled ? 1 : 0,
        sa::engine_sound_hooks_installed() ? 1 : 0,
        s.device_started ? 1 : 0,
        c.hrtf_enabled ? 1 : 0,
        c.occlusion_enabled ? 1 : 0,
        c.reverb_enabled ? 1 : 0,
        c.reverb_wet,
        c.master_volume);
    const sa::Preset* preset = sa::matching_preset(c);
    con_printf("  preset: %s | voice directivity %s | weather %s, volume %.2f | %s\n", preset ? preset->name : "(custom)",
               on_off(c.voice_directivity), on_off(c.weather_occlusion), c.weather_volume,
               s.held ? "waiting for the engine's sound device" : s.silent ? "silent (in the background)"
               : !s.focused ? (c.mute_in_background ? "in the background, fading out" : "in the background, playing on")
               : s.game_paused ? "game paused" : "playing");
    con_printf("  device: %s'%s', opened again %u times, Steam Audio rebuilt %u times%s\n", s.device_started ? "" : "(none) last was ",
               s.device_name.c_str(), s.device_changes, s.dsp_rebuilds, s.rebuild_pending ? ", change pending" : "");
    if (s.reverb_faults > 0) con_printf("  warning: Steam Audio's reverb failed in %u blocks (they were left out)\n", s.reverb_faults);
    con_printf("  HRTF file: %s%s%s\n", s.hrtf_name.c_str(), s.hrtf_warning.empty() ? "" : " | warning: ", s.hrtf_warning.c_str());
    con_printf(
        "  scene: map='%s' tris=%u | ch3d=%u ch2d=%u voices=%u | blocks=%llu sim=%llu | RT60=%.2f/%.2f/%.2f s\n",
        s.loaded_map.c_str(),
        s.scene_triangles,
        s.active_3d_channels,
        s.active_2d_channels,
        s.voices,
        static_cast<unsigned long long>(s.blocks_rendered),
        static_cast<unsigned long long>(s.sim_ticks),
        s.reverb_time[0], s.reverb_time[1], s.reverb_time[2]);
    con_printf(
        "  doors: %s, %d brush entities known, %u solid ones in the scene (%u brush models in the map), %u moves, %u commits\n",
        c.dynamic_doors ? "on" : "off", sa::tracked_brush_count(), s.brush_entities, s.brush_models, s.brush_moves, s.scene_commits);
    con_printf(
        "  reverb: %s, %.2f s early reflections | sky above the listener: %s%.0f %% | raytracing: direct %.1f ms, reflections %.1f ms\n",
        s.reverb_mode == sa::ReverbMode::kHybrid ? "hybrid" : "parametric", c.early_reflections,
        s.map_has_sky ? "" : "(map without sky) ", 100.0f * s.sky_exposure,
        static_cast<float>(s.sim_us_direct) / 1000.0f, static_cast<float>(s.sim_us_reflections) / 1000.0f);
    con_printf("  rain: covers %.0f %% (strength %.2f) | bass guard %.1f dB, limiter %.1f dB | %u glass triangles%s\n",
               100.0f * s.rain, c.rain_masking, s.bass_cut_db, s.limiter_db, s.glass_triangles,
               c.dialogue_glass ? "" : " (off)");
    const float pose_ms = static_cast<float>(s.pose_age_us) / 1000.0f;
    const float block_ms = ms_at(s.frame_size, s.output_rate);
    const float period_ms = ms_at(s.device_period_frames, s.device_rate);
    const float queued_ms = ms_at(s.queued_frames_avg, s.device_rate);
    con_printf(
        "  rates: game %d Hz -> Steam Audio %u Hz -> device %u Hz%s\n",
        sa::kDryRate, s.output_rate, s.device_rate,
        s.output_rate == s.device_rate ? " (no conversion by Windows)" : " (converted by miniaudio)");
    con_printf(
        "  camera -> ears: pose age %.1f ms + DSP block %.1f ms + WASAPI queue %.1f ms (max %.1f) + period %.1f ms = %.1f ms\n",
        pose_ms, block_ms, queued_ms, ms_at(s.queued_frames_max, s.device_rate), period_ms,
        pose_ms + block_ms + queued_ms + period_ms);
    con_printf(
        "  WASAPI: buffer %u frames (%.1f ms), period %u, renders at <= %u queued, late wake-ups %u | DSP block %u frames, worst %.2f ms\n",
        s.device_buffer_frames, ms_at(s.device_buffer_frames, s.device_rate), s.device_period_frames, s.reserve_frames,
        s.late_wakes, s.frame_size, static_cast<float>(s.render_us_max) / 1000.0f);
    con_printf(
        "  dry audio queued %.0f ms (engine paints %.0f ms ahead; never delays direction) | stalls=%u resyncs=%u dropped=%u\n",
        dry_ms(s.dry_buffered_frames), dry_ms(s.engine_ahead_frames), s.starvations, s.resyncs, s.dropped_chunks);
}

void BLSE_CALL cmd_enable(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->is_enabled();
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_enabled(next);
    if (next) sa::clear_vanilla_audio_buffer();
    con_printf("sa_enable = %d (%s)\n", next ? 1 : 0, next ? "Steam Audio + WASAPI" : "Vanilla DirectSound");
}

void BLSE_CALL cmd_hrtf(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().hrtf_enabled;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_hrtf_enabled(next);
    con_printf("sa_hrtf = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_occlusion(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().occlusion_enabled;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_occlusion_enabled(next);
    con_printf("sa_occlusion = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_reverb(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().reverb_enabled;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_reverb_enabled(next);
    if (argc >= 3) {
        g_audio->set_reverb_wet(static_cast<float>(std::atof(argv[2])));
    }
    const auto c = g_audio->config();
    con_printf("sa_reverb = %d (wet=%.2f)\n", c.reverb_enabled ? 1 : 0, c.reverb_wet);
}

void BLSE_CALL cmd_volume(int argc, const char* const* argv, const char*, void*) {
    if (argc >= 2) {
        g_audio->set_master_volume(static_cast<float>(std::atof(argv[1])));
    }
    con_printf("sa_volume = %.2f\n", g_audio->config().master_volume);
}

void BLSE_CALL cmd_reload_bsp(int, const char* const*, const char*, void*) {
    std::string summary;
    if (load_bsp_acoustics_for_current_map(g_current_map.c_str(), summary)) {
        con_printf("sa_reload_bsp: OK (%s)\n", summary.c_str());
    } else {
        con_printf("sa_reload_bsp: failed (%s)\n", summary.c_str());
    }
}

// ---- sa_debug ----

ULONGLONG g_next_debug_dump = 0;

void print_line(const char* line) { con_printf("%s", line); }

void dump_debug() {
    con_printf("---- sa_debug ----\n");
    sa::dump_channels(print_line);
    const std::vector<sa::BrushEntityInfo> brushes = g_audio->brush_entity_infos();
    con_printf("  brush entities: %d known, %u solid in the scene\n", sa::tracked_brush_count(),
               static_cast<unsigned>(brushes.size()));
    std::size_t shown = 0;
    for (const sa::BrushEntityInfo& b : brushes) {
        if (++shown > 24) {
            con_printf("    ... and %u more\n", static_cast<unsigned>(brushes.size() - 24));
            break;
        }
        con_printf("    ent %d '*%d' origin %.1f %.1f %.1f angles %.1f %.1f %.1f\n", b.entity, b.model_index,
                   b.origin[0], b.origin[1], b.origin[2], b.angles[0], b.angles[1], b.angles[2]);
    }
    cmd_status(0, nullptr, nullptr, nullptr);
}

void debug_frame() { // game main thread, after every rendered frame
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_debug_dump) return;
    g_next_debug_dump = now + 1000;
    dump_debug();
}

void BLSE_CALL cmd_debug(int argc, const char* const* argv, const char*, void*) {
    if (argc < 2) {
        dump_debug();
        return;
    }
    const bool on = std::atoi(argv[1]) != 0;
    g_next_debug_dump = 0;
    sa::set_frame_callback(on ? debug_frame : nullptr);
    con_printf("sa_debug = %d%s\n", on ? 1 : 0, on ? " (a dump every second; sa_debug 0 stops it)" : "");
}

void BLSE_CALL cmd_doors(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().dynamic_doors;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_dynamic_doors(next);
    con_printf("sa_doors = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_weather(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().weather_occlusion;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_weather_occlusion(next);
    con_printf("sa_weather = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_weather_vol(int argc, const char* const* argv, const char*, void*) {
    if (argc >= 2) g_audio->set_weather_volume(static_cast<float>(std::atof(argv[1])));
    con_printf("sa_weather_vol = %.2f (0.0 to 2.0)\n", g_audio->config().weather_volume);
}

void BLSE_CALL cmd_rain(int argc, const char* const* argv, const char*, void*) {
    if (argc >= 2) g_audio->set_rain_masking(static_cast<float>(std::atof(argv[1])));
    con_printf("sa_rain = %.2f (0.0 to 2.0); it rains %.0f %% here\n", g_audio->config().rain_masking,
               100.0f * g_audio->get_stats().rain);
}

// One command for all corrections, named as in the settings file.
void BLSE_CALL cmd_tweaks(int argc, const char* const* argv, const char*, void*) {
    static const struct {
        const char* name;
        bool sa::EngineConfig::* value;
    } kTweaks[] = {
        { "dialogue_glass", &sa::EngineConfig::dialogue_glass },
        { "fix_npc_barks", &sa::EngineConfig::fix_npc_barks },
        { "level_voices", &sa::EngineConfig::level_voices },
        { "tame_loud_sounds", &sa::EngineConfig::tame_loud_sounds },
        { "fix_ambient_directions", &sa::EngineConfig::fix_ambient_directions },
        { "filter_heels_ambients", &sa::EngineConfig::filter_heels_ambients },
        { "filter_hoof_ambient", &sa::EngineConfig::filter_hoof_ambient },
    };
    sa::EngineConfig cfg = g_audio->config();
    const char* name = argc >= 2 ? argv[1] : "";
    if (_stricmp(name, "soften_loud_barks") == 0) name = "level_voices"; // as it was called before
    for (const auto& t : kTweaks) {
        if (_stricmp(name, t.name) != 0) continue;
        cfg.*t.value = argc >= 3 ? std::atoi(argv[2]) != 0 : !(cfg.*t.value);
        g_audio->set_tweaks(cfg);
        con_printf("sa_tweaks %s = %d\n", t.name, cfg.*t.value ? 1 : 0);
        return;
    }
    if (argc >= 2) con_printf("sa_tweaks: no tweak '%s'\n", argv[1]);
    for (const auto& t : kTweaks) con_printf("  %-23s %d\n", t.name, cfg.*t.value ? 1 : 0);
    con_printf("  usage: sa_tweaks <name> [0/1]; sa_save keeps it for the next start.\n");
}

void BLSE_CALL cmd_directivity(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().voice_directivity;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_voice_directivity(next);
    con_printf("sa_directivity = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_mute_background(int argc, const char* const* argv, const char*, void*) {
    bool next = !g_audio->config().mute_in_background;
    if (argc >= 2) next = std::atoi(argv[1]) != 0;
    g_audio->set_mute_in_background(next);
    con_printf("sa_mute_background = %d\n", next ? 1 : 0);
}

void BLSE_CALL cmd_reverb_mode(int argc, const char* const* argv, const char*, void*) {
    if (argc < 2) {
        con_printf("sa_reverb_mode = %s (hybrid or parametric)\n",
                   g_audio->get_stats().reverb_mode == sa::ReverbMode::kHybrid ? "hybrid" : "parametric");
        return;
    }
    sa::ReverbMode mode;
    if (_stricmp(argv[1], "hybrid") == 0 || std::strcmp(argv[1], "1") == 0) mode = sa::ReverbMode::kHybrid;
    else if (_stricmp(argv[1], "parametric") == 0 || std::strcmp(argv[1], "0") == 0) mode = sa::ReverbMode::kParametric;
    else {
        con_printf("sa_reverb_mode: unknown mode '%s' (hybrid or parametric)\n", argv[1]);
        return;
    }
    g_audio->request_reverb_mode(mode); // the result follows as a message
}

void BLSE_CALL cmd_sofa(int argc, const char* const* argv, const char*, void*) {
    if (argc < 2) {
        const sa::EngineStats s = g_audio->get_stats();
        con_printf("sa_sofa: HRTF in use: %s%s%s\n", s.hrtf_name.c_str(), s.hrtf_warning.empty() ? "" : " | warning: ",
                   s.hrtf_warning.c_str());
        const std::vector<std::string> files = sa::list_sofa_files(g_mod_dir);
        for (const std::string& f : files) con_printf("  %s\n", f.c_str());
        con_printf("  usage: sa_sofa <file.sofa> | sa_sofa default%s\n",
                   files.empty() ? " (no .sofa files in this mod's folder or its folder \"sofa\")" : "");
        return;
    }
    std::string file; // a path with spaces, in quotes or not
    for (int i = 1; i < argc; ++i) {
        if (i > 1) file += ' ';
        file += argv[i];
    }
    if (_stricmp(file.c_str(), "default") == 0) {
        g_audio->request_hrtf(std::string(), std::wstring());
        return;
    }
    std::wstring path;
    if (!sa::resolve_sofa(g_mod_dir, file, path)) {
        con_printf("sa_sofa: '%s' was not found in '%s' or its folder \"sofa\"; nothing changed.\n", file.c_str(),
                   sa::narrow(g_mod_dir).c_str());
        return;
    }
    g_audio->request_hrtf(file, path); // the result follows as a message
}

void BLSE_CALL cmd_preset(int argc, const char* const* argv, const char*, void*) {
    std::size_t count = 0;
    const sa::Preset* all = sa::presets(count);
    sa::EngineConfig cfg = g_audio->config();
    const sa::Preset* preset = argc >= 2 ? sa::find_preset(argv[1]) : nullptr;
    if (!preset) {
        if (argc >= 2) con_printf("sa_preset: no preset '%s'\n", argv[1]);
        const sa::Preset* current = sa::matching_preset(cfg);
        for (std::size_t i = 0; i < count; ++i) {
            con_printf("  %c %-11s %s\n", &all[i] == current ? '*' : ' ', all[i].name, all[i].description);
        }
        con_printf("  usage: sa_preset <name>; sa_save keeps it for the next start.\n");
        return;
    }
    sa::apply_preset(*preset, cfg);
    g_audio->apply_settings(cfg);
    con_printf("sa_preset = %s: HRTF %s, reverb %s (%.2f), walls %s, doors %s, weather %s, voice directivity %s\n", preset->name,
               on_off(cfg.hrtf_enabled), on_off(cfg.reverb_enabled), cfg.reverb_wet, on_off(cfg.occlusion_enabled),
               on_off(cfg.dynamic_doors), on_off(cfg.weather_occlusion), on_off(cfg.voice_directivity));
}

void BLSE_CALL cmd_save(int, const char* const*, const char*, void*) {
    if (g_mod_dir.empty()) {
        con_printf("sa_save: this mod's folder is not known; nothing saved.\n");
        return;
    }
    const std::wstring path = g_mod_dir + L"\\auspex_audio.ini";
    std::string error;
    if (!sa::save_settings(path, g_audio->config(), error)) {
        con_printf("sa_save: failed: %s\n", error.c_str());
        logf("auspex_audio: sa_save failed: %s", error.c_str());
        return;
    }
    con_printf("sa_save: settings saved to %s\n", sa::narrow(path).c_str());
}

} // namespace

extern "C" BLSE_EXPORT int BLSE_CALL BLSE_PluginLoad(const BLSE_HostAPI* host) {
    if (!host || host->api_version != BLSE_API_VERSION) return 0;
    if (!BLSE_HOST_HAS(host, hook_function)) {
        host->log("auspex_audio requires BLSE with hook_function support.");
        return 0;
    }

    g_host = host;
    g_split_role = BLSE_HOST_HAS(host, split_role) ? host->split_role(host->context) : BLSE_SPLIT_NONE;

    // Audio only runs on the Single or Screen process (never on headless World process in Process Split).
    if (g_split_role == BLSE_SPLIT_WORLD) {
        logf("auspex_audio: Process Split world role detected; staying inactive.");
        return 1;
    }

    // Pre-load phonon.dll from our own plugin directory so delay-loaded Steam Audio symbols resolve
    // cleanly without requiring any DLLs in the game root directory.
    {
        HMODULE self = nullptr;
        wchar_t dll_path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&BLSE_PluginLoad), &self) &&
            GetModuleFileNameW(self, dll_path, MAX_PATH) > 0) {
            std::wstring dir(dll_path);
            g_mod_dir = sa::folder_above(dir, 2);
            const std::size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                dir.resize(slash + 1);
                const std::wstring phonon_path = dir + L"phonon.dll";
                if (!LoadLibraryW(phonon_path.c_str())) {
                    logf("auspex_audio: note: phonon.dll not found next to plugin DLL (trying default DLL search path).");
                }
            }
        }
    }

    const sa::EngineConfig cfg = load_config();
    std::string err;
    g_audio = new (std::nothrow) sa::SteamAudioEngine();
    if (!g_audio) {
        logf("auspex_audio: out of memory.");
        return 0;
    }
    if (!g_audio->init(cfg, /*headless=*/false, err)) {
        logf("auspex_audio: Steam Audio / miniaudio init failed: %s", err.c_str());
        delete g_audio;
        g_audio = nullptr;
        return 0;
    }

    if (!sa::install_engine_sound_hooks(host, g_audio)) {
        logf("auspex_audio: engine.dll hook installation failed; unloading safely.");
        g_audio->shutdown(); // not under the loader lock here: stops and joins both threads before BLSE unloads us
        delete g_audio;
        g_audio = nullptr;
        return 0;
    }
    {
        // Hooks and threads run this DLL's code from now on: it must stay mapped until the process ends.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&BLSE_PluginLoad), &self);
    }
    if (cfg.enabled) {
        sa::clear_vanilla_audio_buffer();
    }

    host->subscribe(host->context, BLSE_MSG_LEVEL_INIT, on_level_init, nullptr);
    host->subscribe(host->context, BLSE_MSG_LEVEL_SHUTDOWN, on_level_shutdown, nullptr);
    host->subscribe(host->context, BLSE_MSG_ENGINE_RESTART, on_engine_restart, nullptr);

    if (BLSE_HOST_HAS(host, add_command)) {
        host->add_command(host->context, "sa_status",     "Show VTMB Steam Audio engine status.", cmd_status, nullptr);
        host->add_command(host->context, "sa_enable",     "Toggle Steam Audio engine (0=vanilla, 1=Steam Audio).", cmd_enable, nullptr);
        host->add_command(host->context, "sa_hrtf",       "Toggle Steam Audio HRTF binaural rendering (0/1).", cmd_hrtf, nullptr);
        host->add_command(host->context, "sa_occlusion",  "Toggle BSP raytraced wall occlusion & transmission (0/1).", cmd_occlusion, nullptr);
        host->add_command(host->context, "sa_reverb",     "Toggle BSP raytraced reflections/reverb (0/1 [wet]).", cmd_reverb, nullptr);
        host->add_command(host->context, "sa_doors",      "Toggle doors and other moving brushes as sound obstacles (0/1).", cmd_doors, nullptr);
        host->add_command(host->context, "sa_weather",    "Toggle muffling of rain under roofs (0/1).", cmd_weather, nullptr);
        host->add_command(host->context, "sa_debug",      "List every playing sound, doors, reverb and latency (1 = every second, 0 = stop).", cmd_debug, nullptr);
        host->add_command(host->context, "sa_volume",     "Get/set Steam Audio master volume multiplier.", cmd_volume, nullptr);
        host->add_command(host->context, "sa_weather_vol", "Get/set the loudness of rain outdoors (0.0 to 2.0).", cmd_weather_vol, nullptr);
        host->add_command(host->context, "sa_rain",       "Get/set how much rain covers distant sounds (0.0 to 2.0).", cmd_rain, nullptr);
        host->add_command(host->context, "sa_tweaks",     "List/set the corrections of the game's own sounds: <name> [0/1].", cmd_tweaks, nullptr);
        host->add_command(host->context, "sa_directivity", "Toggle voices being softer and duller from behind (0/1).", cmd_directivity, nullptr);
        host->add_command(host->context, "sa_reverb_mode", "Get/set the reverb (hybrid or parametric).", cmd_reverb_mode, nullptr);
        host->add_command(host->context, "sa_sofa",       "List/load a custom HRTF: <file.sofa>, or default for the built-in one.", cmd_sofa, nullptr);
        host->add_command(host->context, "sa_preset",     "List/apply a preset: headphones, speakers, subtle, cinematic.", cmd_preset, nullptr);
        host->add_command(host->context, "sa_save",       "Save the current settings to auspex_audio.ini.", cmd_save, nullptr);
        host->add_command(host->context, "sa_mute_background", "Toggle silence while another window is in front (0/1).", cmd_mute_background, nullptr);
        host->add_command(host->context, "sa_reload_bsp", "Reload BSP acoustic geometry from engine.dll.", cmd_reload_bsp, nullptr);
    }

    logf("auspex_audio: loaded (Steam Audio 4.8 + miniaudio WASAPI, enabled=%d, hrtf=%d, occ=%d, reverb=%d)",
         cfg.enabled ? 1 : 0, cfg.hrtf_enabled ? 1 : 0, cfg.occlusion_enabled ? 1 : 0, cfg.reverb_enabled ? 1 : 0);
    {
        const sa::EngineStats s = g_audio->get_stats();
        logf("auspex_audio: HRTF runs in the WASAPI callback (%u Hz, block %u, period %u of %u frames, %s reverb), "
             "independent of snd_mixahead.",
             s.output_rate, s.frame_size, s.device_period_frames, s.device_buffer_frames,
             s.reverb_mode == sa::ReverbMode::kHybrid ? "hybrid" : "parametric");
        logf("auspex_audio: output device '%s', HRTF '%s'.", s.device_name.c_str(), s.hrtf_name.c_str());
        if (!s.hrtf_warning.empty()) {
            logf("auspex_audio: warning: the HRTF file %s; using the built-in HRTF.", s.hrtf_warning.c_str());
        }
    }
    return 1;
}
