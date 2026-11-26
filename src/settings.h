#pragma once

#include "engine_config.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace sa {

// ---- Presets (sa_preset) ----

struct Preset {
    const char* name;
    const char* description;
    bool  hrtf;
    bool  occlusion;
    bool  dynamic_doors;
    bool  reverb;
    float reverb_wet;
    bool  weather_occlusion;
    bool  voice_directivity;
};

const Preset* presets(std::size_t& count);
// Case-insensitive; nullptr: no such preset.
const Preset* find_preset(const char* name);
// Changes only what a preset is about; volumes, HRTF file, quality and output settings stay.
void apply_preset(const Preset& preset, EngineConfig& cfg);
// The preset these settings are exactly, or nullptr.
const Preset* matching_preset(const EngineConfig& cfg);

// ---- auspex_audio.ini ----

// Returns false if the file has no such key.
using IniGetter = std::function<bool(const char* section, const char* key, std::string& value)>;
// Every key of the file, clamped to its valid range; missing and empty ones keep their default.
EngineConfig read_config(const IniGetter& get);

// The whole settings file, commented. `existing` is the file as it is now: keys this version does not know are kept.
std::string format_ini(const EngineConfig& cfg, const std::string& existing = std::string());

// Looks a key up in ini text, the way BLSE does (case-insensitive, ';' and '#' comment lines).
bool ini_lookup(const std::string& text, const char* section, const char* key, std::string& value);

// ---- Files ----

// UTF-8 if it is valid, else the system code page.
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text); // UTF-8

bool read_file(const std::wstring& path, std::string& out, std::size_t max_bytes);
// Through a temporary file next to it, so a crash or a full disk never leaves half a settings file.
bool write_file_atomic(const std::wstring& path, const std::string& text, std::string& error);
// sa_save: the settings file at `path`, new or as it is now, with these settings.
bool save_settings(const std::wstring& path, const EngineConfig& cfg, std::string& error);

// The folder `levels` above the one a file is in, without trailing backslash ("" if there is none).
std::wstring folder_above(const std::wstring& file, int levels);

// Finds a custom HRTF: an absolute path as it is, else <mod_dir>\<file>, else <mod_dir>\sofa\<file>; ".sofa" may be
// left out. False: there is no such file.
bool resolve_sofa(const std::wstring& mod_dir, const std::string& file, std::wstring& path);
// The .sofa files in <mod_dir> and <mod_dir>\sofa, as sa_sofa takes them.
std::vector<std::string> list_sofa_files(const std::wstring& mod_dir);

} // namespace sa
