#pragma once

#include <phonon.h>
#include <cstdint>
#include <string>
#include <vector>

namespace sa {

struct AcousticBrushPlane {
    float nx, ny, nz;
    float dist;
    IPLint32 material_index;
    bool sky = false; // this side shows the sky
};

enum AcousticMaterialId : IPLint32 {
    kMatDefault  = 0, // Generic interior plaster/drywall
    kMatConcrete = 1, // Concrete, brick, stone, asphalt, sewer walls
    kMatWood     = 2, // Wood panels, doors, floors (passes low bass)
    kMatMetal    = 3, // Metal grates, doors, vents, pipes
    kMatGlass    = 4, // Windows, glass partitions
    kMatCarpet   = 5, // Carpet, cloth, soil, grass, foliage
    kMatTile     = 6, // Hard tile, marble (bright reflections)
    kMatCount    = 7
};

// Brush model "*N": a door, elevator, breakable... in its own space (the entity's origin and angles place it).
struct AcousticSubModel {
    int model_index = 0;
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> material_indices;
    std::vector<IPLVector3> glass_vertices;    // its panes of glass once more (also part of `triangles`)
    std::vector<IPLTriangle> glass_triangles;
};

struct AcousticMeshData {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> material_indices;
    std::vector<IPLMaterial> materials;
    std::vector<AcousticSubModel> submodels;   // only those with solid brushes
    std::vector<IPLVector3> sky_vertices;      // faces of the world that show the sky: no obstacle for sound,
    std::vector<IPLTriangle> sky_triangles;    // but where a ray that looks for the open sky arrives
    std::vector<IPLVector3> glass_vertices;    // see-through panes of glass once more (also part of `triangles`):
    std::vector<IPLTriangle> glass_triangles;  // counters, windows, display cases
    std::string map_name;

    void clear() { *this = AcousticMeshData(); }

    bool empty() const { return triangles.empty(); }
};

// Returns the built-in acoustic material table (kMatCount entries).
std::vector<IPLMaterial> default_acoustic_materials();

// Maps a Source texture/surface name (e.g. "CONCRETE/FLOOR01", "WOOD/DOOR_02")
// to an AcousticMaterialId.
IPLint32 classify_surface_material(const char* surface_name);

// Sky, triggers, hints, clips...: brushes that are no obstacle for sound (surface_flags are the BSP's SURF_* bits).
bool surface_lets_sound_through(const char* surface_name, int surface_flags);

// A see-through pane, not a lamp. Windows painted on a wall are none either: their brushes are not CONTENTS_WINDOW.
bool surface_is_pane(const char* surface_name);

// The sky itself (SURF_SKY, SURF_SKY2D or the skybox tool texture).
bool surface_is_sky(const char* surface_name, int surface_flags);

// Pure geometry helper (offline-testable): clips a convex set of half-spaces
// (n . p <= dist in Source units) into outward-facing Steam Audio triangles (Y-up meters).
void clip_brush_to_acoustic_mesh(
    const AcousticBrushPlane* planes,
    int plane_count,
    AcousticMeshData& out_mesh);

// Same, into a brush model.
void clip_brush_to_acoustic_mesh(
    const AcousticBrushPlane* planes,
    int plane_count,
    AcousticSubModel& out_model);

// A brush that shows the sky: adds only its sky faces, to mesh.sky_triangles.
void clip_sky_brush(
    const AcousticBrushPlane* planes,
    int plane_count,
    AcousticMeshData& out_mesh);

// A pane of glass: what rays that look for glass only (GlassProbe) meet.
void clip_glass_brush(const AcousticBrushPlane* planes, int plane_count, std::vector<IPLVector3>& vertices,
                      std::vector<IPLTriangle>& triangles);

// Reads engine.dll's already-loaded CCollisionBSPData (verified RVA 0x34AAF0)
// and extracts all solid worldspawn brushes, brush models and sky faces into Steam Audio mesh data.
bool extract_acoustic_mesh_from_engine(
    AcousticMeshData& out_mesh,
    std::string& out_summary);

// Parses a VTMB Source BSP v17 file directly from disk as a fallback.
bool extract_acoustic_mesh_from_bsp_file(
    const char* bsp_path,
    AcousticMeshData& out_mesh,
    std::string& out_summary);

// Resolves maps/<map_name>.bsp in the running game directory and extracts its mesh.
bool extract_acoustic_mesh_for_map_name(
    const char* map_name,
    AcousticMeshData& out_mesh,
    std::string& out_summary);

} // namespace sa
