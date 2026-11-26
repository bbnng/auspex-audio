#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sa {
namespace {

constexpr Preset kPresets[] = {
    // name, description, hrtf, occlusion, doors, reverb, wet, weather, directivity
    { "headphones", "Balanced 3D sound for headphones (the default).",
      true, true, true, true, 0.55f, true, true },
    { "speakers", "For loudspeakers: stereo instead of HRTF, less reverb (your own room adds some).",
      false, true, true, true, 0.30f, true, true },
    { "subtle", "Close to the original game's clarity: little reverb, voices equally clear from all sides.",
      true, true, true, true, 0.28f, true, false },
    { "cinematic", "Rich, spacious rooms.",
      true, true, true, true, 0.80f, true, true },
};

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool same_text(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

struct IniPair { std::string section, key, value; };

std::vector<IniPair> parse_ini(const std::string& text) {
    std::vector<IniPair> pairs;
    std::string section;
    std::size_t at = (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0) ? 3 : 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(at, nl - at));
        at = nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const std::size_t end = line.find(']');
            if (end != std::string::npos) section = trim(line.substr(1, end - 1));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        pairs.push_back({ section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)) });
    }
    return pairs;
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
    return buf;
}
std::string number(int v) { return std::to_string(v); }
std::string number(bool v) { return v ? "1" : "0"; }

std::string one_line(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; }), s.end());
    return s;
}

float parse_float(const std::string& s, float fallback) {
    const float v = static_cast<float>(std::atof(s.c_str()));
    return std::isfinite(v) ? v : fallback;
}

struct IniEntry {
    const char* section;
    const char* key;
    std::string value;
    const char* comment; // lines separated by '\n'
};

std::vector<IniEntry> ini_entries(const EngineConfig& c) {
    return {
        { "General", "enabled", number(c.enabled),
          "Master enable for the Steam Audio + miniaudio sound engine (1 = enabled, 0 = vanilla DirectSound).\n"
          "In-game: sa_enable 0 / 1" },
        { "General", "master_volume", number(c.master_volume),
          "Master output volume multiplier (0.0 to 2.0, default 1.0). In-game: sa_volume 1.0" },
        { "General", "mute_in_background", number(c.mute_in_background),
          "Fade to silence while another window is in front, as the original game does (1 = on, 0 = keep playing).\n"
          "In-game: sa_mute_background 0 / 1" },

        { "Spatial", "hrtf", number(c.hrtf_enabled),
          "Steam Audio HRTF binaural 3D sound, for headphones (1 = on, 0 = stereo, for loudspeakers). In-game: sa_hrtf 0 / 1" },
        { "Spatial", "sofa_file", one_line(c.sofa_file),
          "Your own HRTF as a .sofa file (empty = the built-in HRTF). Put the file into this mod's folder, or into a folder\n"
          "\"sofa\" in it, and write its name here; a full path works too. In-game: sa_sofa" },
        { "Spatial", "occlusion", number(c.occlusion_enabled),
          "Walls muffle sounds behind them, raytraced and by material (1 = on, 0 = off). In-game: sa_occlusion 0 / 1" },
        { "Spatial", "occlusion_samples", number(c.occlusion_samples),
          "Number of occlusion rays per sound (4 to 64, default 16)." },
        { "Spatial", "dynamic_doors", number(c.dynamic_doors),
          "Doors, elevators, breakable walls and other moving brushes block sound while they are closed and let it through\n"
          "when they open (1 = on, 0 = only the map itself). In-game: sa_doors 0 / 1" },
        { "Spatial", "voice_directivity", number(c.voice_directivity),
          "Characters sound a little softer and duller when they face away from you (1 = on, 0 = off). Lines of a\n"
          "conversation with you are never affected. In-game: sa_directivity 0 / 1" },
        { "Spatial", "dialogue_glass", number(c.dialogue_glass),
          "The protective glass of counters (pawn shop, blood bank) muffles the voice behind it, also in conversations\n"
          "(1 = on, 0 = off). In-game: sa_tweaks dialogue_glass 0 / 1" },
        { "Spatial", "reverb", number(c.reverb_enabled),
          "Raytraced reflections and reverb of the room you are in (1 = on, 0 = off). In-game: sa_reverb 0 / 1" },
        { "Spatial", "reverb_mode", c.reverb_mode == ReverbMode::kHybrid ? "hybrid" : "parametric",
          "hybrid = the first reflections come from the walls they bounce off (convolution), the rest is a smooth tail.\n"
          "parametric = smooth tail only, a little cheaper. If hybrid cannot be set up, parametric is used by itself.\n"
          "In-game: sa_reverb_mode hybrid / parametric" },
        { "Spatial", "early_reflections", number(c.early_reflections),
          "Hybrid reverb only: how many seconds are rendered as single reflections (0.1 to 0.6, default 0.3). Longer costs\n"
          "more CPU." },
        { "Spatial", "reverb_wet", number(c.reverb_wet),
          "Loudness of the reverb (0.0 to 2.0, default 0.55). In-game: sa_reverb 1 0.55" },
        { "Spatial", "reflection_rays", number(c.reflection_rays),
          "Max reflection rays per simulation step (256 to 4096, default 1024)." },
        { "Spatial", "reflection_bounces", number(c.reflection_bounces),
          "Max reflection bounces (1 to 8, default 4)." },
        { "Spatial", "reverb_duration", number(c.reverb_duration),
          "Longest simulated reverb in seconds (0.5 to 3.0, default 1.25)." },

        { "Weather", "weather_occlusion", number(c.weather_occlusion),
          "Rain and thunder that play everywhere become quieter and duller under a roof (1 = on, 0 = equally loud\n"
          "everywhere). In-game: sa_weather 0 / 1" },
        { "Weather", "weather_volume", number(c.weather_volume),
          "Loudness of that rain outdoors, compared to the original game (0.0 to 2.0, default 0.65).\n"
          "In-game: sa_weather_vol 0.65" },
        { "Weather", "rain_masking", number(c.rain_masking),
          "How much rain covers what is far away while you stand in it: distant voices and steps get softer and duller,\n"
          "traffic and other sounds of the city step back (0.0 = not at all, default 1.0, up to 2.0). Near voices and\n"
          "conversations stay clear. In-game: sa_rain 1.0" },

        { "Tweaks", "fix_npc_barks", number(c.fix_npc_barks),
          "Lines of passers-by that the game plays equally in both ears come from the person who says them (1 = on, 0 = off).\n"
          "All of [Tweaks] in-game: sa_tweaks" },
        { "Tweaks", "level_voices", number(c.level_voices),
          "Everybody speaks at one loudness: passers-by and people in conversations who were recorded too loud are turned\n"
          "down, those who are too quiet are turned up. Works with any voice file, also of mods. Whispers stay soft, screams\n"
          "stay loud (1 = on, 0 = the recordings as they are)." },
        { "Tweaks", "tame_loud_sounds", number(c.tame_loud_sounds),
          "Frenzy, and bass that stacks up (combat music, disciplines), stay below the level where they hurt\n"
          "(1 = on, 0 = off)." },
        { "Tweaks", "fix_ambient_directions", number(c.fix_ambient_directions),
          "Outdoors, random street sounds (steps, cars, sirens) come from the street instead of out of the walls of houses\n"
          "(1 = on, 0 = off)." },
        { "Tweaks", "filter_heels_ambients", number(c.filter_heels_ambients),
          "Leaves out the heels and steps of people who are not there: background sounds of streets, cafes and clubs\n"
          "(Pedestrianby1, 2, 3, 5 and Ambient_High_Heels). Steps of the player and of characters always stay\n"
          "(1 = leave out, 0 = play them)." },
        { "Tweaks", "filter_hoof_ambient", number(c.filter_hoof_ambient),
          "Leaves out the trotting background steps that sound like the hoofs of a horse (Pedestrianby4)\n"
          "(1 = leave out, 0 = play them)." },

        { "Output", "sample_rate", number(c.sample_rate),
          "0 = the rate of your sound device (best: no conversion by Windows, lowest delay). 44100 or 48000 force that rate." },
    };
}

bool file_exists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_absolute(const std::wstring& p) {
    return (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
           (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/'));
}

bool has_sofa_extension(const std::wstring& p) {
    return p.size() > 5 && _wcsicmp(p.c_str() + p.size() - 5, L".sofa") == 0;
}

} // namespace

const Preset* presets(std::size_t& count) {
    count = sizeof(kPresets) / sizeof(kPresets[0]);
    return kPresets;
}

const Preset* find_preset(const char* name) {
    if (!name) return nullptr;
    for (const Preset& p : kPresets) {
        if (_stricmp(p.name, name) == 0) return &p;
    }
    return nullptr;
}

void apply_preset(const Preset& p, EngineConfig& cfg) {
    cfg.hrtf_enabled = p.hrtf;
    cfg.occlusion_enabled = p.occlusion;
    cfg.dynamic_doors = p.dynamic_doors;
    cfg.reverb_enabled = p.reverb;
    cfg.reverb_wet = p.reverb_wet;
    cfg.weather_occlusion = p.weather_occlusion;
    cfg.voice_directivity = p.voice_directivity;
}

const Preset* matching_preset(const EngineConfig& cfg) {
    for (const Preset& p : kPresets) {
        if (cfg.hrtf_enabled == p.hrtf && cfg.occlusion_enabled == p.occlusion && cfg.dynamic_doors == p.dynamic_doors &&
            cfg.reverb_enabled == p.reverb && std::fabs(cfg.reverb_wet - p.reverb_wet) < 0.005f &&
            cfg.weather_occlusion == p.weather_occlusion && cfg.voice_directivity == p.voice_directivity) {
            return &p;
        }
    }
    return nullptr;
}

EngineConfig read_config(const IniGetter& get) {
    EngineConfig cfg{};
    std::string v;
    const auto has = [&](const char* section, const char* key) {
        v.clear();
        return get(section, key, v) && !v.empty();
    };
    const auto flag = [&](const char* section, const char* key, bool& out) {
        if (has(section, key)) out = std::atoi(v.c_str()) != 0;
    };
    const auto whole = [&](const char* section, const char* key, int& out) {
        if (has(section, key)) out = std::atoi(v.c_str());
    };
    const auto real = [&](const char* section, const char* key, float& out) {
        if (has(section, key)) out = parse_float(v, out);
    };
    flag("General", "enabled", cfg.enabled);
    real("General", "master_volume", cfg.master_volume);
    flag("General", "mute_in_background", cfg.mute_in_background);
    flag("Spatial", "hrtf", cfg.hrtf_enabled);
    if (has("Spatial", "sofa_file")) cfg.sofa_file = v;
    flag("Spatial", "occlusion", cfg.occlusion_enabled);
    whole("Spatial", "occlusion_samples", cfg.occlusion_samples);
    flag("Spatial", "dynamic_doors", cfg.dynamic_doors);
    flag("Spatial", "voice_directivity", cfg.voice_directivity);
    flag("Spatial", "dialogue_glass", cfg.dialogue_glass);
    flag("Spatial", "reverb", cfg.reverb_enabled);
    if (has("Spatial", "reverb_mode")) {
        cfg.reverb_mode = (same_text(v, "parametric") || v == "0") ? ReverbMode::kParametric : ReverbMode::kHybrid;
    }
    real("Spatial", "early_reflections", cfg.early_reflections);
    real("Spatial", "reverb_wet", cfg.reverb_wet);
    whole("Spatial", "reflection_rays", cfg.reflection_rays);
    whole("Spatial", "reflection_bounces", cfg.reflection_bounces);
    real("Spatial", "reverb_duration", cfg.reverb_duration);
    flag("Weather", "weather_occlusion", cfg.weather_occlusion);
    real("Weather", "weather_volume", cfg.weather_volume);
    real("Weather", "rain_masking", cfg.rain_masking);
    flag("Tweaks", "fix_npc_barks", cfg.fix_npc_barks);
    flag("Tweaks", "soften_loud_barks", cfg.level_voices); // as it was called before
    flag("Tweaks", "level_voices", cfg.level_voices);
    flag("Tweaks", "tame_loud_sounds", cfg.tame_loud_sounds);
    flag("Tweaks", "fix_ambient_directions", cfg.fix_ambient_directions);
    flag("Tweaks", "filter_heels_ambients", cfg.filter_heels_ambients);
    flag("Tweaks", "filter_hoof_ambient", cfg.filter_hoof_ambient);
    whole("Output", "sample_rate", cfg.sample_rate);

    cfg.master_volume      = std::clamp(cfg.master_volume, 0.0f, 2.0f);
    cfg.occlusion_samples  = std::clamp(cfg.occlusion_samples, 4, 64);
    cfg.reverb_wet         = std::clamp(cfg.reverb_wet, 0.0f, 2.0f);
    cfg.reflection_rays    = std::clamp(cfg.reflection_rays, 256, 4096);
    cfg.reflection_bounces = std::clamp(cfg.reflection_bounces, 1, 8);
    cfg.reverb_duration    = std::clamp(cfg.reverb_duration, 0.5f, 3.0f);
    cfg.early_reflections  = std::clamp(cfg.early_reflections, 0.1f, 0.6f);
    cfg.weather_volume     = std::clamp(cfg.weather_volume, 0.0f, 2.0f);
    cfg.rain_masking       = std::clamp(cfg.rain_masking, 0.0f, 2.0f);
    if (cfg.sample_rate != 44100 && cfg.sample_rate != 48000) cfg.sample_rate = 0;
    if (same_text(cfg.sofa_file, "default")) cfg.sofa_file.clear();
    return cfg;
}

std::string format_ini(const EngineConfig& cfg, const std::string& existing) {
    const std::vector<IniEntry> entries = ini_entries(cfg);
    const auto known = [&](const IniPair& p) {
        for (const IniEntry& e : entries) {
            if (same_text(p.section, e.section) && same_text(p.key, e.key)) return true;
        }
        // Keys of older versions that others took the place of.
        return same_text(p.section, "Tweaks") && (same_text(p.key, "soften_loud_barks") || same_text(p.key, "filter_odd_ambients"));
    };
    std::vector<IniPair> others;
    for (const IniPair& p : parse_ini(existing)) {
        if (!known(p) && !p.section.empty()) others.push_back(p);
    }

    std::string out;
    const auto write_others = [&](const std::string& section) { // keys of other versions, where they were
        bool first = true;
        for (IniPair& p : others) {
            if (p.key.empty() || !same_text(p.section, section)) continue;
            if (first) out += "; Not known to this version, kept as it was:\r\n";
            first = false;
            out += p.key + "=" + p.value + "\r\n";
            p.key.clear();
        }
        if (!first) out += "\r\n";
    };
    std::string section;
    for (const IniEntry& e : entries) {
        if (section != e.section) {
            if (!section.empty()) write_others(section);
            section = e.section;
            out += "[" + section + "]\r\n";
        }
        for (const char* p = e.comment; *p;) {
            const char* nl = std::strchr(p, '\n');
            const std::size_t n = nl ? static_cast<std::size_t>(nl - p) : std::strlen(p);
            out += "; " + std::string(p, n) + "\r\n";
            p += n + (nl ? 1 : 0);
        }
        out += std::string(e.key) + "=" + e.value + "\r\n\r\n";
    }
    write_others(section);
    for (;;) { // sections this version does not have at all
        std::string next;
        for (const IniPair& p : others) {
            if (!p.key.empty()) {
                next = p.section;
                break;
            }
        }
        if (next.empty()) break;
        out += "[" + next + "]\r\n";
        write_others(next);
    }
    while (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) out.resize(out.size() - 2);
    return out;
}

bool ini_lookup(const std::string& text, const char* section, const char* key, std::string& value) {
    for (const IniPair& p : parse_ini(text)) {
        if (same_text(p.section, section) && same_text(p.key, key)) {
            value = p.value;
            return true;
        }
    }
    return false;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    const int size = static_cast<int>(text.size());
    int n = MultiByteToWideChar(page, flags, text.data(), size, nullptr, 0);
    if (n <= 0) {
        page = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(page, flags, text.data(), size, nullptr, 0);
    }
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(page, flags, text.data(), size, &out[0], n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = static_cast<int>(text.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), size, &out[0], n, nullptr, nullptr);
    return out;
}

bool read_file(const std::wstring& path, std::string& out, std::size_t max_bytes) {
    out.clear();
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= max_bytes;
    if (ok && size.QuadPart > 0) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
        if (!ok) out.clear();
    }
    CloseHandle(h);
    return ok;
}

bool write_file_atomic(const std::wstring& path, const std::string& text, std::string& error) {
    const std::wstring temp = path + L".new";
    const auto fail = [&](const char* what) {
        error = std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    };
    const HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("the file cannot be created");
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size() &&
                    FlushFileBuffers(h);
    if (!ok) fail("the file cannot be written");
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fail("the settings file cannot be replaced");
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool save_settings(const std::wstring& path, const EngineConfig& cfg, std::string& error) {
    std::string existing;
    if (file_exists(path) && !read_file(path, existing, 1u << 20)) {
        error = "the old settings file cannot be read"; // it may hold keys of another version: do not overwrite it
        return false;
    }
    return write_file_atomic(path, format_ini(cfg, existing), error);
}

std::wstring folder_above(const std::wstring& file, int levels) {
    std::wstring dir = file;
    for (int i = 0; i <= levels; ++i) {
        const std::size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return std::wstring();
        dir.resize(slash);
    }
    return dir;
}

bool resolve_sofa(const std::wstring& mod_dir, const std::string& file, std::wstring& path) {
    path.clear();
    std::wstring name = widen(file);
    if (name.size() >= 2 && name.front() == L'"' && name.back() == L'"') name = name.substr(1, name.size() - 2);
    if (name.empty()) return false;
    std::vector<std::wstring> places;
    if (is_absolute(name)) {
        places.push_back(name);
    } else if (!mod_dir.empty()) {
        places.push_back(mod_dir + L"\\" + name);
        places.push_back(mod_dir + L"\\sofa\\" + name);
    }
    for (const std::wstring& p : places) {
        if (file_exists(p)) {
            path = p;
            return true;
        }
        if (!has_sofa_extension(p) && file_exists(p + L".sofa")) {
            path = p + L".sofa";
            return true;
        }
    }
    return false;
}

std::vector<std::string> list_sofa_files(const std::wstring& mod_dir) {
    std::vector<std::string> names;
    if (mod_dir.empty()) return names;
    for (const wchar_t* sub : { L"\\", L"\\sofa\\" }) {
        WIN32_FIND_DATAW data{};
        const HANDLE h = FindFirstFileW((mod_dir + sub + L"*.sofa").c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && has_sofa_extension(data.cFileName)) {
                names.push_back(narrow(data.cFileName));
            }
        } while (names.size() < 64 && FindNextFileW(h, &data));
        FindClose(h);
    }
    return names;
}

} // namespace sa
